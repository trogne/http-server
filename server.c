#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <sqlite3.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define SERVER_NAME "dense-http/2.0"
#define MAX_THREADS 256
#define MAX_QUEUE 4096
#define MAX_HEADER_BYTES 16384
#define MAX_TARGET_BYTES 4096
#define MAX_KEEPALIVE_REQUESTS 100
#define MAX_BODY_BYTES 1048576

typedef struct {
    uint16_t port;
    size_t threads;
    size_t queue_capacity;
    int keepalive_timeout;
    size_t keepalive_requests;
    char document_root[4096];
    char log_file[4096];
    char database_path[4096];
    char template_root[4096];
} server_config;

typedef struct {
    int *items;
    size_t capacity, head, tail, count;
    bool stopping;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
} connection_queue;

typedef struct {
    char method[16];
    char target[MAX_TARGET_BYTES];
    int major, minor;
    bool keep_alive;
    bool head_only;
    size_t content_length;
    char content_type[128];
    char authorization[512];
    char *body;
} http_request;

static server_config config = {
    .port = 8080, .threads = 4, .queue_capacity = 256,
    .keepalive_timeout = 5, .keepalive_requests = MAX_KEEPALIVE_REQUESTS,
    .document_root = "./public", .log_file = "-",
    .database_path = "./app.db", .template_root = "./templates"
};
static connection_queue queue;
static volatile sig_atomic_t stopping;
static volatile sig_atomic_t listener = -1;
static _Atomic unsigned long long request_count;
static _Atomic unsigned long long active_connections;
static time_t started_at;
static FILE *access_log;
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
static sqlite3 *database;
static pthread_mutex_t database_mutex = PTHREAD_MUTEX_INITIALIZER;
static char api_token[256];

static void on_signal(int signum) {
    (void)signum;
    stopping = 1;
    if (listener >= 0) {
        close((int)listener);
        listener = -1;
    }
}

static char *trim(char *text) {
    while (isspace((unsigned char)*text)) text++;
    char *end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) *--end = '\0';
    return text;
}

static bool contains_case_insensitive(const char *haystack, const char *needle) {
    size_t length = strlen(needle);
    for (; *haystack; haystack++)
        if (!strncasecmp(haystack, needle, length)) return true;
    return false;
}

static bool number(const char *text, unsigned long min, unsigned long max, unsigned long *out) {
    char *end;
    errno = 0;
    unsigned long value = strtoul(text, &end, 10);
    if (errno || end == text || *end || value < min || value > max) return false;
    *out = value;
    return true;
}

static bool copy_setting(char *out, size_t capacity, const char *value) {
    size_t length = strlen(value);
    if (length == 0 || length >= capacity) return false;
    memcpy(out, value, length + 1);
    return true;
}

static bool apply_setting(const char *key, const char *value, const char *source, size_t line) {
    unsigned long parsed;
    bool ok = true;
    if (!strcmp(key, "port")) {
        ok = number(value, 1, 65535, &parsed); if (ok) config.port = (uint16_t)parsed;
    } else if (!strcmp(key, "threads")) {
        ok = number(value, 1, MAX_THREADS, &parsed); if (ok) config.threads = (size_t)parsed;
    } else if (!strcmp(key, "queue_capacity")) {
        ok = number(value, 1, MAX_QUEUE, &parsed); if (ok) config.queue_capacity = (size_t)parsed;
    } else if (!strcmp(key, "keepalive_timeout")) {
        ok = number(value, 1, 300, &parsed); if (ok) config.keepalive_timeout = (int)parsed;
    } else if (!strcmp(key, "keepalive_requests")) {
        ok = number(value, 1, 10000, &parsed); if (ok) config.keepalive_requests = (size_t)parsed;
    } else if (!strcmp(key, "document_root")) {
        ok = copy_setting(config.document_root, sizeof(config.document_root), value);
    } else if (!strcmp(key, "log_file")) {
        ok = copy_setting(config.log_file, sizeof(config.log_file), value);
    } else if (!strcmp(key, "database_path")) {
        ok = copy_setting(config.database_path, sizeof(config.database_path), value);
    } else if (!strcmp(key, "template_root")) {
        ok = copy_setting(config.template_root, sizeof(config.template_root), value);
    } else {
        fprintf(stderr, "%s:%zu: unknown setting '%s'\n", source, line, key);
        return false;
    }
    if (!ok) fprintf(stderr, "%s:%zu: invalid value for %s\n", source, line, key);
    return ok;
}

static bool load_config(const char *path) {
    FILE *file = fopen(path, "r");
    if (!file) { perror(path); return false; }
    char *line = NULL;
    size_t capacity = 0, line_number = 0;
    bool valid = true;
    while (getline(&line, &capacity, file) >= 0) {
        line_number++;
        char *entry = trim(line);
        if (!*entry || *entry == '#') continue;
        char *equals = strchr(entry, '=');
        if (!equals) {
            fprintf(stderr, "%s:%zu: expected key = value\n", path, line_number);
            valid = false; continue;
        }
        *equals = '\0';
        if (!apply_setting(trim(entry), trim(equals + 1), path, line_number)) valid = false;
    }
    if (ferror(file)) { perror(path); valid = false; }
    free(line);
    fclose(file);
    return valid;
}

static bool queue_init(void) {
    queue.items = calloc(config.queue_capacity, sizeof(*queue.items));
    if (!queue.items) return false;
    queue.capacity = config.queue_capacity;
    if (pthread_mutex_init(&queue.mutex, NULL) || pthread_cond_init(&queue.not_empty, NULL)) {
        free(queue.items); return false;
    }
    return true;
}

static bool enqueue(int fd) {
    bool accepted = false;
    pthread_mutex_lock(&queue.mutex);
    if (!queue.stopping && queue.count < queue.capacity) {
        queue.items[queue.tail] = fd;
        queue.tail = (queue.tail + 1) % queue.capacity;
        queue.count++;
        pthread_cond_signal(&queue.not_empty);
        accepted = true;
    }
    pthread_mutex_unlock(&queue.mutex);
    return accepted;
}

static int dequeue(void) {
    pthread_mutex_lock(&queue.mutex);
    while (!queue.count && !queue.stopping) pthread_cond_wait(&queue.not_empty, &queue.mutex);
    if (!queue.count) { pthread_mutex_unlock(&queue.mutex); return -1; }
    int fd = queue.items[queue.head];
    queue.head = (queue.head + 1) % queue.capacity;
    queue.count--;
    pthread_mutex_unlock(&queue.mutex);
    return fd;
}

static void stop_workers(void) {
    pthread_mutex_lock(&queue.mutex);
    queue.stopping = true;
    pthread_cond_broadcast(&queue.not_empty);
    pthread_mutex_unlock(&queue.mutex);
}

static bool send_all(int fd, const void *buffer, size_t length) {
    const char *data = buffer;
    while (length) {
        ssize_t sent = send(fd, data, length, MSG_NOSIGNAL);
        if (sent < 0 && errno == EINTR) continue;
        if (sent <= 0) return false;
        data += (size_t)sent; length -= (size_t)sent;
    }
    return true;
}

static void http_date(char output[30], time_t value) {
    struct tm tm;
    gmtime_r(&value, &tm);
    (void)strftime(output, 30, "%a, %d %b %Y %H:%M:%S GMT", &tm);
}

static bool send_headers(int fd, int status, const char *reason, const char *type,
                         off_t length, bool keep_alive, const char *extra) {
    char date[30], headers[1024];
    http_date(date, time(NULL));
    int size = snprintf(headers, sizeof(headers),
        "HTTP/1.1 %d %s\r\nDate: %s\r\nServer: %s\r\n"
        "Content-Type: %s\r\nContent-Length: %lld\r\n"
        "Connection: %s\r\n%s%s\r\n",
        status, reason, date, SERVER_NAME, type, (long long)length,
        keep_alive ? "keep-alive" : "close", extra ? extra : "", extra ? "\r\n" : "");
    return size > 0 && (size_t)size < sizeof(headers) && send_all(fd, headers, (size_t)size);
}

static bool send_text(int fd, int status, const char *reason, const char *type,
                      const char *body, bool head, bool keep_alive, const char *extra) {
    size_t length = strlen(body);
    return send_headers(fd, status, reason, type, (off_t)length, keep_alive, extra) &&
           (head || send_all(fd, body, length));
}

static const char *mime_type(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    struct mime { const char *ext, *type; };
    static const struct mime types[] = {
        {".html", "text/html; charset=utf-8"}, {".htm", "text/html; charset=utf-8"},
        {".css", "text/css; charset=utf-8"}, {".js", "text/javascript; charset=utf-8"},
        {".json", "application/json"}, {".txt", "text/plain; charset=utf-8"},
        {".xml", "application/xml"}, {".svg", "image/svg+xml"}, {".png", "image/png"},
        {".jpg", "image/jpeg"}, {".jpeg", "image/jpeg"}, {".gif", "image/gif"},
        {".webp", "image/webp"}, {".ico", "image/x-icon"}, {".pdf", "application/pdf"},
        {".wasm", "application/wasm"}, {".woff", "font/woff"}, {".woff2", "font/woff2"},
        {".mp4", "video/mp4"}, {".webm", "video/webm"}, {".mp3", "audio/mpeg"}
    };
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++)
        if (!strcasecmp(dot, types[i].ext)) return types[i].type;
    return "application/octet-stream";
}

static int hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool safe_path(const char *target, char *output, size_t capacity) {
    if (*target != '/') return false;
    size_t used = 0;
    for (size_t i = 1; target[i] && target[i] != '?'; i++) {
        unsigned char c = (unsigned char)target[i];
        if (c == '%') {
            int hi = hex(target[i + 1]), lo = target[i + 2];
            lo = target[i + 2] ? hex(target[i + 2]) : -1;
            if (hi < 0 || lo < 0) return false;
            c = (unsigned char)((hi << 4) | lo); i += 2;
        }
        if (!c || c == '\\' || c < 0x20 || used + 1 >= capacity) return false;
        output[used++] = (char)c;
    }
    output[used] = '\0';
    char check[MAX_TARGET_BYTES];
    if (used >= sizeof(check)) return false;
    memcpy(check, output, used + 1);
    char *save = NULL;
    for (char *part = strtok_r(check, "/", &save); part; part = strtok_r(NULL, "/", &save))
        if (!strcmp(part, "..") || !strcmp(part, ".")) return false;
    return true;
}

static void log_request(const struct sockaddr_in *peer, const http_request *request,
                        int status, off_t bytes, long elapsed_us) {
    char address[INET_ADDRSTRLEN] = "-", stamp[64];
    (void)inet_ntop(AF_INET, &peer->sin_addr, address, sizeof(address));
    time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm);
    (void)strftime(stamp, sizeof(stamp), "%d/%b/%Y:%H:%M:%S %z", &tm);
    pthread_mutex_lock(&log_mutex);
    fprintf(access_log, "%s - - [%s] \"%s %s HTTP/%d.%d\" %d %lld %ldus\n",
            address, stamp, request->method, request->target, request->major, request->minor,
            status, (long long)bytes, elapsed_us);
    fflush(access_log);
    pthread_mutex_unlock(&log_mutex);
}

static int parse_request(char *header, http_request *request) {
    char *line_end = strstr(header, "\r\n");
    if (!line_end) return 400;
    *line_end = '\0';
    char extra;
    if (sscanf(header, "%15s %4095s HTTP/%d.%d%c", request->method, request->target,
               &request->major, &request->minor, &extra) != 4) return 400;
    if (request->major != 1 || (request->minor != 0 && request->minor != 1)) return 505;
    request->head_only = !strcmp(request->method, "HEAD");
    request->keep_alive = request->minor == 1;
    bool host = false, content_length = false, transfer_encoding = false;
    char *cursor = line_end + 2;
    while (*cursor) {
        char *end = strstr(cursor, "\r\n");
        if (!end) return 400;
        if (end == cursor) break;
        *end = '\0';
        char *colon = strchr(cursor, ':');
        if (!colon || colon == cursor) return 400;
        *colon = '\0';
        for (char *p = cursor; *p; p++)
            if (!isalnum((unsigned char)*p) && *p != '-') return 400;
        char *value = trim(colon + 1);
        if (!strcasecmp(cursor, "Host")) host = *value != '\0';
        else if (!strcasecmp(cursor, "Connection")) {
            if (contains_case_insensitive(value, "close")) request->keep_alive = false;
            else if (contains_case_insensitive(value, "keep-alive")) request->keep_alive = true;
        } else if (!strcasecmp(cursor, "Content-Length")) {
            unsigned long length;
            if (!number(value, 0, MAX_BODY_BYTES, &length) || content_length) return 400;
            request->content_length = (size_t)length; content_length = true;
        } else if (!strcasecmp(cursor, "Content-Type")) {
            if (strlen(value) >= sizeof(request->content_type)) return 400;
            memcpy(request->content_type, value, strlen(value) + 1);
        } else if (!strcasecmp(cursor, "Authorization")) {
            if (strlen(value) >= sizeof(request->authorization) || request->authorization[0]) return 400;
            memcpy(request->authorization, value, strlen(value) + 1);
        } else if (!strcasecmp(cursor, "Transfer-Encoding")) transfer_encoding = true;
        cursor = end + 2;
    }
    if (request->minor == 1 && !host) return 400;
    if (transfer_encoding) return 501;
    if (strcmp(request->method, "GET") && strcmp(request->method, "HEAD") &&
        strcmp(request->method, "POST") && strcmp(request->method, "PUT") &&
        strcmp(request->method, "DELETE")) return 405;
    if ((!strcmp(request->method, "POST") || !strcmp(request->method, "PUT")) &&
        !content_length) return 411;
    return 0;
}

typedef struct { char *data; size_t length, capacity; } string_buffer;

static bool append_bytes(string_buffer *out, const char *data, size_t length) {
    if (length > SIZE_MAX - out->length - 1) return false;
    size_t needed = out->length + length + 1;
    if (needed > out->capacity) {
        size_t capacity = out->capacity ? out->capacity : 1024;
        while (capacity < needed) {
            if (capacity > SIZE_MAX / 2) { capacity = needed; break; }
            capacity *= 2;
        }
        char *grown = realloc(out->data, capacity);
        if (!grown) return false;
        out->data = grown; out->capacity = capacity;
    }
    memcpy(out->data + out->length, data, length);
    out->length += length; out->data[out->length] = '\0';
    return true;
}

static bool append_html(string_buffer *out, const char *text) {
    for (; *text; text++) {
        const char *replacement = NULL;
        if (*text == '&') replacement = "&amp;";
        else if (*text == '<') replacement = "&lt;";
        else if (*text == '>') replacement = "&gt;";
        else if (*text == '\"') replacement = "&quot;";
        else if (*text == '\'') replacement = "&#39;";
        if (replacement) { if (!append_bytes(out, replacement, strlen(replacement))) return false; }
        else if (!append_bytes(out, text, 1)) return false;
    }
    return true;
}

static char *read_file(const char *path, size_t *length) {
    FILE *file = fopen(path, "rb");
    if (!file || fseek(file, 0, SEEK_END)) { if (file) fclose(file); return NULL; }
    long size = ftell(file);
    if (size < 0 || fseek(file, 0, SEEK_SET)) { fclose(file); return NULL; }
    char *data = malloc((size_t)size + 1);
    if (!data) { fclose(file); return NULL; }
    size_t got = fread(data, 1, (size_t)size, file); fclose(file);
    if (got != (size_t)size) { free(data); return NULL; }
    data[got] = '\0'; *length = got; return data;
}

/* Tiny template language: {{name}} HTML-escapes; {{{name}}} inserts trusted HTML. */
static char *render_template(const char *name, const char *title, const char *content, size_t *length) {
    char path[8192];
    if (snprintf(path, sizeof(path), "%s/%s", config.template_root, name) >= (int)sizeof(path)) return NULL;
    size_t source_length; char *source = read_file(path, &source_length);
    if (!source) return NULL;
    string_buffer out = {0}; size_t cursor = 0;
    while (cursor < source_length) {
        char *open = strstr(source + cursor, "{{");
        if (!open) { append_bytes(&out, source + cursor, source_length - cursor); break; }
        append_bytes(&out, source + cursor, (size_t)(open - source - cursor));
        bool raw = open[2] == '{'; const char *close = strstr(open + (raw ? 3 : 2), raw ? "}}}" : "}}");
        if (!close) { append_bytes(&out, open, strlen(open)); break; }
        size_t key_length = (size_t)(close - open) - (raw ? 3U : 2U);
        const char *value = "";
        if (key_length == 5 && !strncmp(open + (raw ? 3 : 2), "title", 5)) value = title;
        else if (key_length == 7 && !strncmp(open + (raw ? 3 : 2), "content", 7)) value = content;
        if (raw) append_bytes(&out, value, strlen(value)); else append_html(&out, value);
        cursor = (size_t)(close - source) + (raw ? 3U : 2U);
    }
    free(source);
    if (!out.data) out.data = calloc(1, 1);
    *length = out.length; return out.data;
}

static bool form_value(const char *body, const char *key, char *out, size_t capacity) {
    size_t key_length = strlen(key);
    for (const char *p = body; *p; p = strchr(p, '&') ? strchr(p, '&') + 1 : p + strlen(p)) {
        if (strncmp(p, key, key_length) || p[key_length] != '=') continue;
        p += key_length + 1; size_t used = 0;
        while (*p && *p != '&') {
            unsigned char c = (unsigned char)*p++;
            if (c == '+') c = ' ';
            else if (c == '%' && p[0] && p[1]) {
                int hi = hex(p[0]), lo = hex(p[1]); if (hi < 0 || lo < 0) return false;
                c = (unsigned char)((hi << 4) | lo); p += 2;
            }
            if (!c || used + 1 >= capacity) return false;
            out[used++] = (char)c;
        }
        out[used] = '\0'; return used > 0;
    }
    return false;
}

static bool append_json_string(string_buffer *out, const char *text) {
    if (!append_bytes(out, "\"", 1)) return false;
    for (const unsigned char *cursor = (const unsigned char *)text; *cursor; cursor++) {
        const char *replacement = NULL;
        switch (*cursor) {
            case '\\': replacement = "\\\\"; break;
            case '"': replacement = "\\\""; break;
            case '\n': replacement = "\\n"; break;
            case '\r': replacement = "\\r"; break;
            case '\t': replacement = "\\t"; break;
            case '\b': replacement = "\\b"; break;
            case '\f': replacement = "\\f"; break;
            default: break;
        }
        if (replacement) {
            if (!append_bytes(out, replacement, strlen(replacement))) return false;
        } else if (*cursor < 0x20) {
            char escaped[7];
            int length = snprintf(escaped, sizeof(escaped), "\\u%04x", *cursor);
            if (length != 6 || !append_bytes(out, escaped, 6)) return false;
        } else if (!append_bytes(out, (const char *)cursor, 1)) {
            return false;
        }
    }
    return append_bytes(out, "\"", 1);
}

static bool append_user_json(string_buffer *out, int id, const char *name, const char *email) {
    char text[64];
    int length = snprintf(text, sizeof(text), "%d", id);
    if (length < 0 || (size_t)length >= sizeof(text)) return false;
    return append_bytes(out, "{\"id\":", 6) && append_bytes(out, text, (size_t)length) &&
           append_bytes(out, ",\"name\":", 8) && append_json_string(out, name) &&
           append_bytes(out, ",\"email\":", 9) && append_json_string(out, email) &&
           append_bytes(out, "}", 1);
}

static bool parse_json_string(const char **cursor, char *out, size_t capacity) {
    if (**cursor != '"') return false;
    (*cursor)++;
    size_t used = 0;
    while (**cursor && **cursor != '"') {
        char current = *(*cursor)++;
        if ((unsigned char)current < 0x20) return false;
        if (current == '\\') {
            if (!**cursor) return false;
            char escaped = *(*cursor)++;
            switch (escaped) {
                case '"': current = '"'; break;
                case '\\': current = '\\'; break;
                case '/': current = '/'; break;
                case 'b': current = '\b'; break;
                case 'f': current = '\f'; break;
                case 'n': current = '\n'; break;
                case 'r': current = '\r'; break;
                case 't': current = '\t'; break;
                default: return false;
            }
        }
        if (used + 1 >= capacity) return false;
        out[used++] = current;
    }
    if (**cursor != '"') return false;
    (*cursor)++;
    out[used] = '\0';
    return true;
}

static bool is_valid_email(const char *email) {
    const char *at = strchr(email, '@');
    if (!at || at == email) return false;
    const char *dot = strrchr(email, '.');
    if (!dot || dot <= at + 1 || dot[1] == '\0') return false;
    for (const char *cursor = email; *cursor; cursor++) {
        unsigned char c = (unsigned char)*cursor;
        if (isalnum(c) || c == '.' || c == '_' || c == '-' || c == '@') continue;
        return false;
    }
    return true;
}

static bool parse_user_payload(const char *body, char *name, size_t name_capacity,
                               char *email, size_t email_capacity) {
    const char *cursor = body;
    while (*cursor && isspace((unsigned char)*cursor)) cursor++;
    if (*cursor != '{') return false;
    cursor++;
    bool saw_name = false, saw_email = false;
    for (;;) {
        while (*cursor && isspace((unsigned char)*cursor)) cursor++;
        if (*cursor == '}') { cursor++; break; }
        char key[64];
        if (!parse_json_string(&cursor, key, sizeof(key))) return false;
        while (*cursor && isspace((unsigned char)*cursor)) cursor++;
        if (*cursor != ':') return false;
        cursor++;
        while (*cursor && isspace((unsigned char)*cursor)) cursor++;
        if (!strcmp(key, "name")) {
            if (!parse_json_string(&cursor, name, name_capacity)) return false;
            saw_name = true;
        } else if (!strcmp(key, "email")) {
            if (!parse_json_string(&cursor, email, email_capacity)) return false;
            saw_email = true;
        } else {
            char ignored[1024];
            if (!parse_json_string(&cursor, ignored, sizeof(ignored))) return false;
        }
        while (*cursor && isspace((unsigned char)*cursor)) cursor++;
        if (*cursor == ',') { cursor++; continue; }
        if (*cursor == '}') { cursor++; break; }
        return false;
    }
    while (*cursor && isspace((unsigned char)*cursor)) cursor++;
    return saw_name && saw_email && *cursor == '\0';
}

static bool send_json(int fd, int status, const char *reason, const char *body, bool head,
                      bool keep_alive, const char *extra) {
    return send_text(fd, status, reason, "application/json", body, head, keep_alive, extra);
}

static bool secure_equals(const char *left, const char *right) {
    size_t left_length = strlen(left), right_length = strlen(right);
    size_t length = left_length > right_length ? left_length : right_length;
    unsigned char difference = (unsigned char)(left_length ^ right_length);
    for (size_t i = 0; i < length; i++) {
        unsigned char a = i < left_length ? (unsigned char)left[i] : 0;
        unsigned char b = i < right_length ? (unsigned char)right[i] : 0;
        difference |= a ^ b;
    }
    return difference == 0;
}

static int require_user_write_auth(int fd, const http_request *request, bool keep_alive,
                                   off_t *bytes) {
    if (!api_token[0]) {
        const char *body = "{\"error\":\"user writes are disabled until DENSE_HTTP_API_TOKEN is configured\"}\n";
        *bytes = (off_t)strlen(body);
        return send_json(fd, 503, "Service Unavailable", body, false, keep_alive, NULL) ? 503 : -1;
    }
    const char prefix[] = "Bearer ";
    if (strncmp(request->authorization, prefix, sizeof(prefix) - 1) ||
        !secure_equals(request->authorization + sizeof(prefix) - 1, api_token)) {
        const char *body = "{\"error\":\"valid bearer token required\"}\n";
        *bytes = (off_t)strlen(body);
        return send_json(fd, 401, "Unauthorized", body, false, keep_alive,
                         "WWW-Authenticate: Bearer") ? 401 : -1;
    }
    return 0;
}

static bool decode_query_component(const char *encoded, size_t length, char *out, size_t capacity) {
    size_t used = 0;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)encoded[i];
        if (c == '+') {
            c = ' ';
        } else if (c == '%') {
            if (i + 2 >= length) return false;
            int hi = hex(encoded[i + 1]), lo = hex(encoded[i + 2]);
            if (hi < 0 || lo < 0) return false;
            c = (unsigned char)((hi << 4) | lo);
            i += 2;
        }
        if (!c || used + 1 >= capacity) return false;
        out[used++] = (char)c;
    }
    out[used] = '\0';
    return true;
}

static bool parse_query_value(const char *query, const char *key, char *out, size_t capacity) {
    size_t key_length = strlen(key);
    if (!query) return false;
    for (const char *cursor = query; cursor && *cursor; ) {
        const char *separator = strchr(cursor, '&');
        size_t length = separator ? (size_t)(separator - cursor) : strlen(cursor);
        if (length > key_length + 1 && !strncmp(cursor, key, key_length) && cursor[key_length] == '=') {
            size_t value_length = length - key_length - 1;
            return decode_query_component(cursor + key_length + 1, value_length, out, capacity);
        }
        if (!separator) break;
        cursor = separator + 1;
    }
    return false;
}

static unsigned long parse_positive_number(const char *text, unsigned long fallback) {
    unsigned long value = fallback;
    if (!text || !*text) return value;
    char *end = NULL;
    errno = 0;
    unsigned long parsed = strtoul(text, &end, 10);
    if (!errno && end != text && !*end && parsed > 0) value = parsed;
    return value;
}

static bool parse_analysis_payload(const char *body, char *notes, size_t capacity) {
    const char *cursor = body;
    while (*cursor && isspace((unsigned char)*cursor)) cursor++;
    if (*cursor++ != '{') return false;
    bool found = false;
    for (;;) {
        while (*cursor && isspace((unsigned char)*cursor)) cursor++;
        if (*cursor == '}') { cursor++; break; }
        char key[64], value[2048];
        if (!parse_json_string(&cursor, key, sizeof(key))) return false;
        while (*cursor && isspace((unsigned char)*cursor)) cursor++;
        if (*cursor++ != ':') return false;
        while (*cursor && isspace((unsigned char)*cursor)) cursor++;
        if (!parse_json_string(&cursor, value, sizeof(value))) return false;
        if (!strcmp(key, "notes")) {
            size_t length = strlen(value);
            if (!length || length >= capacity || found) return false;
            memcpy(notes, value, length + 1);
            found = true;
        }
        while (*cursor && isspace((unsigned char)*cursor)) cursor++;
        if (*cursor == ',') { cursor++; continue; }
        if (*cursor == '}') { cursor++; break; }
        return false;
    }
    while (*cursor && isspace((unsigned char)*cursor)) cursor++;
    return found && *cursor == '\0';
}

static bool parse_note(const char *text, int *midi, unsigned int *pitch_class) {
    if (!text[0]) return false;
    int pitch;
    switch (toupper((unsigned char)text[0])) {
        case 'C': pitch = 0; break;
        case 'D': pitch = 2; break;
        case 'E': pitch = 4; break;
        case 'F': pitch = 5; break;
        case 'G': pitch = 7; break;
        case 'A': pitch = 9; break;
        case 'B': pitch = 11; break;
        default: return false;
    }
    const char *cursor = text + 1;
    if (*cursor == '#') { pitch++; cursor++; }
    else if (*cursor == 'b' || *cursor == 'B') { pitch--; cursor++; }
    char *end = NULL;
    errno = 0;
    long octave = strtol(cursor, &end, 10);
    if (errno || end == cursor || *end || octave < -1 || octave > 9) return false;
    pitch = (pitch % 12 + 12) % 12;
    long value = (octave + 1) * 12 + pitch;
    if (value < 0 || value > 127) return false;
    *midi = (int)value;
    *pitch_class = (unsigned int)pitch;
    return true;
}

static int analysis_route(int fd, http_request *request, bool keep_alive, off_t *bytes) {
    if (strcmp(request->method, "POST")) {
        const char *body = "{\"error\":\"method not allowed\"}\n"; *bytes = (off_t)strlen(body);
        return send_json(fd, 405, "Method Not Allowed", body, false, keep_alive,
                         "Allow: POST") ? 405 : -1;
    }
    char notes[2048];
    if (!request->body || !parse_analysis_payload(request->body, notes, sizeof(notes))) {
        const char *body = "{\"error\":\"expected JSON with a non-empty notes string\"}\n";
        *bytes = (off_t)strlen(body);
        return send_json(fd, 400, "Bad Request", body, false, keep_alive, NULL) ? 400 : -1;
    }
    size_t count = 0, ascending = 0, descending = 0, repeated = 0;
    unsigned int pitch_classes = 0;
    int lowest = 128, highest = -1, previous = -1;
    char *save = NULL;
    for (char *token = strtok_r(notes, ", \t\r\n", &save); token;
         token = strtok_r(NULL, ", \t\r\n", &save)) {
        int midi; unsigned int pitch_class;
        if (count == 128 || !parse_note(token, &midi, &pitch_class)) {
            const char *body = "{\"error\":\"use 1-128 notes such as C4, F#4, or Bb3\"}\n";
            *bytes = (off_t)strlen(body);
            return send_json(fd, 400, "Bad Request", body, false, keep_alive, NULL) ? 400 : -1;
        }
        if (previous >= 0) {
            if (midi > previous) ascending++;
            else if (midi < previous) descending++;
            else repeated++;
        }
        if (midi < lowest) lowest = midi;
        if (midi > highest) highest = midi;
        pitch_classes |= 1U << pitch_class;
        previous = midi; count++;
    }
    if (!count) {
        const char *body = "{\"error\":\"at least one note is required\"}\n";
        *bytes = (off_t)strlen(body);
        return send_json(fd, 400, "Bad Request", body, false, keep_alive, NULL) ? 400 : -1;
    }
    unsigned int distinct = 0;
    for (unsigned int bits = pitch_classes; bits; bits >>= 1) distinct += bits & 1U;
    const char *contour = ascending > descending ? "predominantly ascending" :
                          descending > ascending ? "predominantly descending" : "balanced";
    char body[512];
    int length = snprintf(body, sizeof(body),
        "{\"note_count\":%zu,\"distinct_pitch_classes\":%u,\"lowest_midi\":%d,"
        "\"highest_midi\":%d,\"range_semitones\":%d,\"ascending\":%zu,"
        "\"descending\":%zu,\"repeated\":%zu,\"contour\":\"%s\"}\n",
        count, distinct, lowest, highest, highest - lowest, ascending, descending, repeated, contour);
    if (length < 0 || (size_t)length >= sizeof(body)) return -1;
    *bytes = (off_t)length;
    return send_json(fd, 200, "OK", body, false, keep_alive, "Cache-Control: no-store") ? 200 : -1;
}

static int users_route(int fd, http_request *request, bool keep_alive, off_t *bytes) {
    char path[MAX_TARGET_BYTES];
    if (!safe_path(request->target, path, sizeof(path))) {
        const char *body = "{\"error\":\"bad request\"}\n"; *bytes = (off_t)strlen(body);
        return send_json(fd, 400, "Bad Request", body, false, keep_alive, NULL) ? 400 : -1;
    }
    char route[MAX_TARGET_BYTES];
    if (!path[0]) {
        route[0] = '/'; route[1] = '\0';
    } else {
        if (strlen(path) + 1 >= sizeof(route)) return -1;
        route[0] = '/'; memcpy(route + 1, path, strlen(path) + 1);
    }

    if (!strcmp(route, "/users") || !strcmp(route, "/users/")) {
        if (!strcmp(request->method, "POST")) {
            int auth_status = require_user_write_auth(fd, request, keep_alive, bytes);
            if (auth_status) return auth_status;
            char name[256] = {0}, email[256] = {0};
            if (!request->body || !parse_user_payload(request->body, name, sizeof(name),
                                                      email, sizeof(email)) ||
                !*name || !*email || !is_valid_email(email)) {
                const char *body = "{\"error\":\"invalid user payload\"}\n"; *bytes = (off_t)strlen(body);
                return send_json(fd, 400, "Bad Request", body, false, keep_alive, NULL) ? 400 : -1;
            }
            pthread_mutex_lock(&database_mutex);
            sqlite3_stmt *statement = NULL;
            bool ok = sqlite3_prepare_v2(database,
                "INSERT INTO users(name, email) VALUES(?, ?)", -1, &statement, NULL) == SQLITE_OK &&
                sqlite3_bind_text(statement, 1, name, -1, SQLITE_TRANSIENT) == SQLITE_OK &&
                sqlite3_bind_text(statement, 2, email, -1, SQLITE_TRANSIENT) == SQLITE_OK &&
                sqlite3_step(statement) == SQLITE_DONE;
            long long id = ok ? sqlite3_last_insert_rowid(database) : 0;
            sqlite3_finalize(statement); pthread_mutex_unlock(&database_mutex);
            if (!ok) {
                const char *body = "{\"error\":\"could not create user\"}\n"; *bytes = (off_t)strlen(body);
                return send_json(fd, 500, "Internal Server Error", body, false, keep_alive, NULL) ? 500 : -1;
            }
            string_buffer payload = {0};
            ok = append_user_json(&payload, (int)id, name, email);
            if (!ok) { free(payload.data); return -1; }
            *bytes = (off_t)payload.length;
            bool sent = send_headers(fd, 201, "Created", "application/json", (off_t)payload.length,
                                     keep_alive, NULL) &&
                        (request->head_only || send_all(fd, payload.data, payload.length));
            free(payload.data);
            return sent ? 201 : -1;
        }
        if (!strcmp(request->method, "GET") || !strcmp(request->method, "HEAD")) {
            char query[256] = {0};
            const char *question = strchr(request->target, '?');
            if (question) {
                size_t length = strlen(question + 1);
                if (length >= sizeof(query)) length = sizeof(query) - 1;
                memcpy(query, question + 1, length);
                query[length] = '\0';
            }
            char page_text[32] = {0}, limit_text[32] = {0}, search_text[256] = {0}, sort_text[32] = {0}, order_text[16] = {0};
            bool has_page = parse_query_value(query, "page", page_text, sizeof(page_text));
            bool has_limit = parse_query_value(query, "limit", limit_text, sizeof(limit_text));
            bool has_search = parse_query_value(query, "search", search_text, sizeof(search_text));
            bool has_sort = parse_query_value(query, "sort", sort_text, sizeof(sort_text));
            bool has_order = parse_query_value(query, "order", order_text, sizeof(order_text));
            unsigned long page = has_page ? parse_positive_number(page_text, 1) : 1;
            unsigned long limit = has_limit ? parse_positive_number(limit_text, 10) : 10;
            if (limit > 100) limit = 100;
            const char *sort_expression = "id";
            const char *direction = "ASC";
            if (has_sort && !strcmp(sort_text, "name")) {
                sort_expression = "lower(name)";
            } else if (has_sort && !strcmp(sort_text, "email")) {
                sort_expression = "lower(email)";
            } else if (has_sort && !strcmp(sort_text, "id")) {
                sort_expression = "id";
            }
            if (has_order && !strcmp(order_text, "desc")) direction = "DESC";
            pthread_mutex_lock(&database_mutex);
            sqlite3_stmt *statement = NULL;
            char sql[1024];
            snprintf(sql, sizeof(sql),
                "SELECT COUNT(*) FROM users%s",
                has_search ? " WHERE lower(name) LIKE ? OR lower(email) LIKE ?" : "");
            bool ok = sqlite3_prepare_v2(database, sql, -1, &statement, NULL) == SQLITE_OK;
            if (has_search) {
                char pattern[512];
                int written = snprintf(pattern, sizeof(pattern), "%%%s%%", search_text);
                if (written < 0 || (size_t)written >= sizeof(pattern)) { ok = false; }
                for (size_t i = 0; pattern[i]; i++) pattern[i] = tolower((unsigned char)pattern[i]);
                if (ok) ok = ok && sqlite3_bind_text(statement, 1, pattern, -1, SQLITE_TRANSIENT) == SQLITE_OK &&
                      sqlite3_bind_text(statement, 2, pattern, -1, SQLITE_TRANSIENT) == SQLITE_OK &&
                      sqlite3_step(statement) == SQLITE_ROW;
            } else {
                ok = ok && sqlite3_step(statement) == SQLITE_ROW;
            }
            unsigned long total = ok ? (unsigned long)sqlite3_column_int(statement, 0) : 0;
            sqlite3_finalize(statement);
            statement = NULL;
            snprintf(sql, sizeof(sql),
                "SELECT id, name, email FROM users%s ORDER BY %s %s LIMIT ? OFFSET ?",
                has_search ? " WHERE lower(name) LIKE ? OR lower(email) LIKE ?" : "",
                sort_expression, direction);
            ok = sqlite3_prepare_v2(database, sql, -1, &statement, NULL) == SQLITE_OK;
            if (has_search) {
                char pattern[512];
                int written = snprintf(pattern, sizeof(pattern), "%%%s%%", search_text);
                if (written < 0 || (size_t)written >= sizeof(pattern)) { ok = false; }
                for (size_t i = 0; pattern[i]; i++) pattern[i] = tolower((unsigned char)pattern[i]);
                if (ok) ok = ok && sqlite3_bind_text(statement, 1, pattern, -1, SQLITE_TRANSIENT) == SQLITE_OK &&
                      sqlite3_bind_text(statement, 2, pattern, -1, SQLITE_TRANSIENT) == SQLITE_OK &&
                      sqlite3_bind_int(statement, 3, (int)limit) == SQLITE_OK &&
                      sqlite3_bind_int(statement, 4, (int)((page - 1) * limit)) == SQLITE_OK;
            } else {
                ok = ok && sqlite3_bind_int(statement, 1, (int)limit) == SQLITE_OK &&
                      sqlite3_bind_int(statement, 2, (int)((page - 1) * limit)) == SQLITE_OK;
            }
            string_buffer payload = {0};
            ok = ok && append_bytes(&payload, "{\"page\":", 8);
            char page_value[32]; int page_length = snprintf(page_value, sizeof(page_value), "%lu", page);
            ok = ok && page_length > 0 && (size_t)page_length < sizeof(page_value) &&
                 append_bytes(&payload, page_value, (size_t)page_length);
            ok = ok && append_bytes(&payload, ",\"limit\":", 9);
            char limit_value[32]; int limit_length = snprintf(limit_value, sizeof(limit_value), "%lu", limit);
            ok = ok && limit_length > 0 && (size_t)limit_length < sizeof(limit_value) &&
                 append_bytes(&payload, limit_value, (size_t)limit_length);
            ok = ok && append_bytes(&payload, ",\"total\":", 9);
            char total_value[32]; int total_length = snprintf(total_value, sizeof(total_value), "%lu", total);
            ok = ok && total_length > 0 && (size_t)total_length < sizeof(total_value) &&
                 append_bytes(&payload, total_value, (size_t)total_length);
            ok = ok && append_bytes(&payload, ",\"users\":[", 10);
            bool first = true;
            int step_result = SQLITE_DONE;
            while (ok && (step_result = sqlite3_step(statement)) == SQLITE_ROW) {
                if (!first) ok = append_bytes(&payload, ",", 1);
                first = false;
                if (ok) ok = append_user_json(&payload, sqlite3_column_int(statement, 0),
                                              (const char *)sqlite3_column_text(statement, 1),
                                              (const char *)sqlite3_column_text(statement, 2));
            }
            if (ok && step_result != SQLITE_DONE) ok = false;
            sqlite3_finalize(statement); pthread_mutex_unlock(&database_mutex);
            ok = ok && append_bytes(&payload, "]}", 2);
            if (!ok) { free(payload.data); return -1; }
            *bytes = (off_t)payload.length;
            bool sent = send_headers(fd, 200, "OK", "application/json", (off_t)payload.length,
                                     keep_alive, NULL) &&
                        (request->head_only || send_all(fd, payload.data, payload.length));
            free(payload.data);
            return sent ? 200 : -1;
        }
        const char *body = "{\"error\":\"method not allowed\"}\n"; *bytes = (off_t)strlen(body);
        return send_json(fd, 405, "Method Not Allowed", body, false, keep_alive, "Allow: GET, HEAD, POST") ? 405 : -1;
    }

    const char *id_text = route + 7;
    if (route[6] != '/' || !*id_text) {
        const char *body = "{\"error\":\"not found\"}\n"; *bytes = (off_t)strlen(body);
        return send_json(fd, 404, "Not Found", body, false, keep_alive, NULL) ? 404 : -1;
    }
    for (const char *cursor = id_text; *cursor; cursor++) {
        if (*cursor == '/') {
            const char *body = "{\"error\":\"not found\"}\n"; *bytes = (off_t)strlen(body);
            return send_json(fd, 404, "Not Found", body, false, keep_alive, NULL) ? 404 : -1;
        }
    }
    unsigned long parsed = 0;
    if (!number(id_text, 1, 2147483647UL, &parsed)) {
        const char *body = "{\"error\":\"invalid id\"}\n"; *bytes = (off_t)strlen(body);
        return send_json(fd, 400, "Bad Request", body, false, keep_alive, NULL) ? 400 : -1;
    }
    int id = (int)parsed;
    if (!strcmp(request->method, "GET") || !strcmp(request->method, "HEAD")) {
        pthread_mutex_lock(&database_mutex);
        sqlite3_stmt *statement = NULL;
        bool ok = sqlite3_prepare_v2(database, "SELECT id, name, email FROM users WHERE id = ?", -1, &statement, NULL) == SQLITE_OK &&
                  sqlite3_bind_int(statement, 1, id) == SQLITE_OK;
        if (ok && sqlite3_step(statement) == SQLITE_ROW) {
            string_buffer payload = {0};
            ok = append_user_json(&payload, sqlite3_column_int(statement, 0),
                                   (const char *)sqlite3_column_text(statement, 1),
                                   (const char *)sqlite3_column_text(statement, 2));
            sqlite3_finalize(statement); pthread_mutex_unlock(&database_mutex);
            if (!ok) { free(payload.data); return -1; }
            *bytes = (off_t)payload.length;
            bool sent = send_headers(fd, 200, "OK", "application/json", (off_t)payload.length,
                                     keep_alive, NULL) &&
                        (request->head_only || send_all(fd, payload.data, payload.length));
            free(payload.data);
            return sent ? 200 : -1;
        }
        sqlite3_finalize(statement); pthread_mutex_unlock(&database_mutex);
        const char *body = "{\"error\":\"user not found\"}\n"; *bytes = (off_t)strlen(body);
        return send_json(fd, 404, "Not Found", body, false, keep_alive, NULL) ? 404 : -1;
    }
    if (!strcmp(request->method, "PUT")) {
        int auth_status = require_user_write_auth(fd, request, keep_alive, bytes);
        if (auth_status) return auth_status;
        char name[256] = {0}, email[256] = {0};
        if (!request->body || !parse_user_payload(request->body, name, sizeof(name),
                                                  email, sizeof(email)) ||
            !*name || !*email || !is_valid_email(email)) {
            const char *body = "{\"error\":\"invalid user payload\"}\n"; *bytes = (off_t)strlen(body);
            return send_json(fd, 400, "Bad Request", body, false, keep_alive, NULL) ? 400 : -1;
        }
        pthread_mutex_lock(&database_mutex);
        sqlite3_stmt *statement = NULL;
        bool ok = sqlite3_prepare_v2(database,
            "UPDATE users SET name = ?, email = ? WHERE id = ?", -1, &statement, NULL) == SQLITE_OK &&
                  sqlite3_bind_text(statement, 1, name, -1, SQLITE_TRANSIENT) == SQLITE_OK &&
                  sqlite3_bind_text(statement, 2, email, -1, SQLITE_TRANSIENT) == SQLITE_OK &&
                  sqlite3_bind_int(statement, 3, id) == SQLITE_OK &&
                  sqlite3_step(statement) == SQLITE_DONE;
        int changed = ok ? sqlite3_changes(database) : 0;
        sqlite3_finalize(statement); pthread_mutex_unlock(&database_mutex);
        if (!ok) {
            const char *body = "{\"error\":\"could not update user\"}\n"; *bytes = (off_t)strlen(body);
            return send_json(fd, 500, "Internal Server Error", body, false, keep_alive, NULL) ? 500 : -1;
        }
        if (changed == 0) {
            const char *body = "{\"error\":\"user not found\"}\n"; *bytes = (off_t)strlen(body);
            return send_json(fd, 404, "Not Found", body, false, keep_alive, NULL) ? 404 : -1;
        }
        string_buffer payload = {0};
        ok = append_user_json(&payload, id, name, email);
        if (!ok) { free(payload.data); return -1; }
        *bytes = (off_t)payload.length;
        bool sent = send_headers(fd, 200, "OK", "application/json", (off_t)payload.length,
                                 keep_alive, NULL) &&
                    (request->head_only || send_all(fd, payload.data, payload.length));
        free(payload.data);
        return sent ? 200 : -1;
    }
    if (!strcmp(request->method, "DELETE")) {
        int auth_status = require_user_write_auth(fd, request, keep_alive, bytes);
        if (auth_status) return auth_status;
        pthread_mutex_lock(&database_mutex);
        sqlite3_stmt *statement = NULL;
        bool ok = sqlite3_prepare_v2(database, "DELETE FROM users WHERE id = ?", -1, &statement, NULL) == SQLITE_OK &&
                  sqlite3_bind_int(statement, 1, id) == SQLITE_OK &&
                  sqlite3_step(statement) == SQLITE_DONE;
        int changed = ok ? sqlite3_changes(database) : 0;
        sqlite3_finalize(statement); pthread_mutex_unlock(&database_mutex);
        if (!ok) {
            const char *body = "{\"error\":\"could not delete user\"}\n"; *bytes = (off_t)strlen(body);
            return send_json(fd, 500, "Internal Server Error", body, false, keep_alive, NULL) ? 500 : -1;
        }
        if (changed == 0) {
            const char *body = "{\"error\":\"user not found\"}\n"; *bytes = (off_t)strlen(body);
            return send_json(fd, 404, "Not Found", body, false, keep_alive, NULL) ? 404 : -1;
        }
        const char *body = "{\"deleted\":true}\n"; *bytes = (off_t)strlen(body);
        return send_json(fd, 200, "OK", body, request->head_only, keep_alive, NULL) ? 200 : -1;
    }
    const char *body = "{\"error\":\"method not allowed\"}\n"; *bytes = (off_t)strlen(body);
    return send_json(fd, 405, "Method Not Allowed", body, false, keep_alive, "Allow: GET, HEAD, PUT, DELETE") ? 405 : -1;
}

static int notes_route(int fd, http_request *request, bool keep_alive, off_t *bytes) {
    if (!strcmp(request->method, "POST")) {
        char text[1024];
        if (!contains_case_insensitive(request->content_type, "application/x-www-form-urlencoded") ||
            !form_value(request->body ? request->body : "", "text", text, sizeof(text))) {
            const char *body = "Expected a non-empty form field named text.\n"; *bytes = (off_t)strlen(body);
            return send_text(fd, 422, "Unprocessable Content", "text/plain; charset=utf-8", body,
                             false, keep_alive, NULL) ? 422 : -1;
        }
        pthread_mutex_lock(&database_mutex);
        sqlite3_stmt *statement = NULL;
        bool ok = sqlite3_prepare_v2(database, "INSERT INTO notes(text) VALUES(?)", -1, &statement, NULL) == SQLITE_OK &&
                  sqlite3_bind_text(statement, 1, text, -1, SQLITE_TRANSIENT) == SQLITE_OK &&
                  sqlite3_step(statement) == SQLITE_DONE;
        sqlite3_finalize(statement); pthread_mutex_unlock(&database_mutex);
        if (!ok) return -1;
        *bytes = 0;
        return send_headers(fd, 303, "See Other", "text/plain; charset=utf-8", 0, keep_alive,
                            "Location: /notes") ? 303 : -1;
    }
    if (strcmp(request->method, "GET") && strcmp(request->method, "HEAD")) {
        const char *body = "Method not allowed.\n"; *bytes = (off_t)strlen(body);
        return send_text(fd, 405, "Method Not Allowed", "text/plain; charset=utf-8", body,
                         false, keep_alive, "Allow: GET, HEAD, POST") ? 405 : -1;
    }
    string_buffer list = {0};
    append_bytes(&list, "<ul>", 4);
    pthread_mutex_lock(&database_mutex);
    sqlite3_stmt *statement = NULL;
    bool ok = sqlite3_prepare_v2(database, "SELECT id, text FROM notes ORDER BY id DESC", -1, &statement, NULL) == SQLITE_OK;
    while (ok && sqlite3_step(statement) == SQLITE_ROW) {
        char prefix[64]; int n = snprintf(prefix, sizeof(prefix), "<li data-id=\"%lld\">", sqlite3_column_int64(statement, 0));
        ok = n > 0 && (size_t)n < sizeof(prefix) && append_bytes(&list, prefix, (size_t)n) &&
             append_html(&list, (const char *)sqlite3_column_text(statement, 1)) && append_bytes(&list, "</li>", 5);
    }
    sqlite3_finalize(statement); pthread_mutex_unlock(&database_mutex);
    ok = ok && append_bytes(&list, "</ul>", 5);
    size_t page_length = 0; char *page = ok ? render_template("notes.html", "Notes", list.data, &page_length) : NULL;
    free(list.data);
    if (!page) {
        const char *body = "Template not found.\n"; *bytes = (off_t)strlen(body);
        return send_text(fd, 500, "Internal Server Error", "text/plain; charset=utf-8", body,
                         request->head_only, keep_alive, NULL) ? 500 : -1;
    }
    *bytes = (off_t)page_length;
    bool sent = send_headers(fd, 200, "OK", "text/html; charset=utf-8", (off_t)page_length, keep_alive, NULL) &&
                (request->head_only || send_all(fd, page, page_length));
    free(page); return sent ? 200 : -1;
}

static int serve_request(int fd, http_request *request, bool keep_alive, off_t *bytes) {
    char relative[MAX_TARGET_BYTES];
    if (!safe_path(request->target, relative, sizeof(relative))) {
        const char *body = "Bad request.\n"; *bytes = (off_t)strlen(body);
        return send_text(fd, 400, "Bad Request", "text/plain; charset=utf-8", body,
                         request->head_only, keep_alive, NULL) ? 400 : -1;
    }
    char route[MAX_TARGET_BYTES];
    if (!relative[0]) {
        route[0] = '/'; route[1] = '\0';
    } else {
        if (strlen(relative) + 1 >= sizeof(route)) return -1;
        route[0] = '/'; memcpy(route + 1, relative, strlen(relative) + 1);
    }
    if (!strcmp(route, "/notes")) return notes_route(fd, request, keep_alive, bytes);
    if (!strcmp(route, "/users") || !strcmp(route, "/users/") ||
        (!strncmp(route, "/users/", 7) && route[7] != '\0'))
        return users_route(fd, request, keep_alive, bytes);
    if (!strcmp(route, "/api/analyze")) return analysis_route(fd, request, keep_alive, bytes);
    if (!strcmp(route, "/health")) {
        if (strcmp(request->method, "GET") && strcmp(request->method, "HEAD")) {
            const char *body = "Method not allowed.\n"; *bytes = (off_t)strlen(body);
            return send_text(fd, 405, "Method Not Allowed", "text/plain; charset=utf-8", body,
                             false, keep_alive, "Allow: GET, HEAD") ? 405 : -1;
        }
        char body[256];
        int length = snprintf(body, sizeof(body),
            "{\"status\":\"ok\",\"uptime_seconds\":%lld,\"requests\":%llu,\"active_connections\":%llu,\"worker_threads\":%zu}\n",
            (long long)(time(NULL) - started_at), atomic_load(&request_count),
            atomic_load(&active_connections), config.threads);
        if (length < 0 || (size_t)length >= sizeof(body)) return -1;
        *bytes = length;
        return send_text(fd, 200, "OK", "application/json", body, request->head_only,
                         keep_alive, NULL) ? 200 : -1;
    }
    if (!strncmp(request->target, "/api/stats", 10) &&
        (request->target[10] == '\0' || request->target[10] == '?')) {
        if (strcmp(request->method, "GET") && strcmp(request->method, "HEAD")) {
            const char *body = "Method not allowed.\n"; *bytes = (off_t)strlen(body);
            return send_text(fd, 405, "Method Not Allowed", "text/plain; charset=utf-8", body,
                             false, keep_alive, "Allow: GET, HEAD") ? 405 : -1;
        }
        char body[256];
        int length = snprintf(body, sizeof(body),
            "{\"status\":\"ok\",\"uptime_seconds\":%lld,\"requests\":%llu,"
            "\"active_connections\":%llu,\"worker_threads\":%zu}\n",
            (long long)(time(NULL) - started_at), atomic_load(&request_count),
            atomic_load(&active_connections), config.threads);
        if (length < 0 || (size_t)length >= sizeof(body)) return -1;
        *bytes = length;
        return send_text(fd, 200, "OK", "application/json", body, request->head_only,
                         keep_alive, "Cache-Control: no-store") ? 200 : -1;
    }
    if (strcmp(request->method, "GET") && strcmp(request->method, "HEAD")) {
        const char *body = "No route matched this method.\n"; *bytes = (off_t)strlen(body);
        return send_text(fd, 405, "Method Not Allowed", "text/plain; charset=utf-8", body,
                         false, keep_alive, "Allow: GET, HEAD") ? 405 : -1;
    }
    if (!*relative || relative[strlen(relative) - 1] == '/') {
        if (strlen(relative) + strlen("index.html") >= sizeof(relative)) return -1;
        strcat(relative, "index.html");
    }
    int file = open(config.document_root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    char path_copy[MAX_TARGET_BYTES];
    memcpy(path_copy, relative, strlen(relative) + 1);
    char *save = NULL;
    for (char *part = strtok_r(path_copy, "/", &save); part && file >= 0;
         part = strtok_r(NULL, "/", &save)) {
        bool last = save == NULL || *save == '\0';
        int flags = O_RDONLY | O_NOFOLLOW | O_CLOEXEC | (last ? 0 : O_DIRECTORY);
        int next = openat(file, part, flags);
        close(file);
        file = next;
    }
    struct stat info;
    if (file < 0 || fstat(file, &info) || !S_ISREG(info.st_mode)) {
        if (file >= 0) close(file);
        const char *body = "Not found.\n"; *bytes = (off_t)strlen(body);
        return send_text(fd, 404, "Not Found", "text/plain; charset=utf-8", body,
                         request->head_only, keep_alive, NULL) ? 404 : -1;
    }
    *bytes = info.st_size;
    char modified[30], extra[128];
    http_date(modified, info.st_mtime);
    (void)snprintf(extra, sizeof(extra), "Last-Modified: %s\r\nX-Content-Type-Options: nosniff", modified);
    bool ok = send_headers(fd, 200, "OK", mime_type(relative), info.st_size, keep_alive, extra);
    if (ok && !request->head_only) {
        off_t offset = 0;
        while (offset < info.st_size) {
            ssize_t sent = sendfile(fd, file, &offset, (size_t)(info.st_size - offset));
            if (sent < 0 && errno == EINTR) continue;
            if (sent <= 0) { ok = false; break; }
        }
    }
    close(file);
    return ok ? 200 : -1;
}

static void handle_connection(int fd, const struct sockaddr_in *peer) {
    struct timeval timeout = {.tv_sec = config.keepalive_timeout};
    int enabled = 1;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    char buffer[MAX_HEADER_BYTES + 1]; size_t used = 0;
    for (size_t sequence = 0; sequence < config.keepalive_requests && !stopping; sequence++) {
        char *end = NULL;
        while (!(end = used >= 4 ? strstr(buffer, "\r\n\r\n") : NULL)) {
            if (used == MAX_HEADER_BYTES) {
                (void)send_text(fd, 431, "Request Header Fields Too Large", "text/plain; charset=utf-8",
                                "Request headers are too large.\n", false, false, NULL);
                return;
            }
            ssize_t received = recv(fd, buffer + used, MAX_HEADER_BYTES - used, 0);
            if (received < 0 && errno == EINTR) continue;
            if (received <= 0) return;
            used += (size_t)received; buffer[used] = '\0';
        }
        size_t header_size = (size_t)(end - buffer) + 4;
        char header[MAX_HEADER_BYTES + 1];
        memcpy(header, buffer, header_size); header[header_size] = '\0';
        memmove(buffer, buffer + header_size, used - header_size);
        used -= header_size; buffer[used] = '\0';
        http_request request = {.major = 1, .minor = 1};
        struct timespec begin, finish; clock_gettime(CLOCK_MONOTONIC, &begin);
        int error = parse_request(header, &request);
        if (!error && request.content_length) {
            request.body = malloc(request.content_length + 1);
            if (!request.body) error = 500;
            size_t copied = used < request.content_length ? used : request.content_length;
            if (!error) memcpy(request.body, buffer, copied);
            if (copied) {
                memmove(buffer, buffer + copied, used - copied);
                used -= copied; buffer[used] = '\0';
            }
            while (!error && copied < request.content_length) {
                ssize_t received = recv(fd, request.body + copied, request.content_length - copied, 0);
                if (received < 0 && errno == EINTR) continue;
                if (received <= 0) { error = 400; break; }
                copied += (size_t)received;
            }
            if (!error) request.body[request.content_length] = '\0';
        }
        bool keep = request.keep_alive && sequence + 1 < config.keepalive_requests;
        int status; off_t bytes = 0;
        if (error) {
            const char *reason = error == 405 ? "Method Not Allowed" : error == 505 ?
                                 "HTTP Version Not Supported" : error == 501 ? "Not Implemented" :
                                 error == 411 ? "Length Required" : error == 500 ? "Internal Server Error" : "Bad Request";
            const char *extra = error == 405 ? "Allow: GET, HEAD, POST, PUT, DELETE" : NULL;
            const char *body = error == 405 ? "Unsupported HTTP method.\n" : "Malformed HTTP request.\n";
            bytes = (off_t)strlen(body);
            (void)send_text(fd, error, reason, "text/plain; charset=utf-8", body, false, false, extra);
            status = error; keep = false;
        } else {
            atomic_fetch_add(&request_count, 1);
            status = serve_request(fd, &request, keep, &bytes);
        }
        free(request.body);
        clock_gettime(CLOCK_MONOTONIC, &finish);
        long elapsed = (finish.tv_sec - begin.tv_sec) * 1000000L +
                       (finish.tv_nsec - begin.tv_nsec) / 1000L;
        if (status > 0) log_request(peer, &request, status, bytes, elapsed);
        if (status < 0 || !keep) return;
    }
}

static void *worker_main(void *unused) {
    (void)unused;
    for (;;) {
        int fd = dequeue();
        if (fd < 0) break;
        struct sockaddr_in peer; socklen_t length = sizeof(peer);
        memset(&peer, 0, sizeof(peer));
        (void)getpeername(fd, (struct sockaddr *)&peer, &length);
        atomic_fetch_add(&active_connections, 1);
        handle_connection(fd, &peer);
        atomic_fetch_sub(&active_connections, 1);
        close(fd);
    }
    return NULL;
}

static int create_listener(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0), enabled = 1;
    if (fd < 0) return -1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(config.port),
                                  .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) || listen(fd, 512)) {
        close(fd); return -1;
    }
    return fd;
}

static void usage(const char *program) {
    fprintf(stderr, "Usage: %s [-c FILE] [--port N] [--threads N]\n", program);
}

int main(int argc, char **argv) {
    const char *config_path = NULL;
    for (int i = 1; i < argc; i++) {
        if ((!strcmp(argv[i], "-c") || !strcmp(argv[i], "--config")) && i + 1 < argc)
            config_path = argv[++i];
        else if ((!strcmp(argv[i], "-p") || !strcmp(argv[i], "--port")) && i + 1 < argc) {
            unsigned long value; if (!number(argv[++i], 1, 65535, &value)) { usage(argv[0]); return 2; }
            config.port = (uint16_t)value;
        } else if ((!strcmp(argv[i], "-t") || !strcmp(argv[i], "--threads")) && i + 1 < argc) {
            unsigned long value; if (!number(argv[++i], 1, MAX_THREADS, &value)) { usage(argv[0]); return 2; }
            config.threads = (size_t)value;
        } else { usage(argv[0]); return 2; }
    }
    if (config_path && !load_config(config_path)) return EXIT_FAILURE;
    const char *token = getenv("DENSE_HTTP_API_TOKEN");
    if (token) {
        size_t length = strlen(token);
        if (length < 16 || length >= sizeof(api_token)) {
            fprintf(stderr, "DENSE_HTTP_API_TOKEN must contain 16 to %zu characters\n",
                    sizeof(api_token) - 1);
            return EXIT_FAILURE;
        }
        memcpy(api_token, token, length + 1);
    }
    /* CLI options intentionally override the configuration file. */
    for (int i = 1; config_path && i < argc; i++) {
        if ((!strcmp(argv[i], "-p") || !strcmp(argv[i], "--port")) && i + 1 < argc) {
            unsigned long value; (void)number(argv[++i], 1, 65535, &value); config.port = (uint16_t)value;
        } else if ((!strcmp(argv[i], "-t") || !strcmp(argv[i], "--threads")) && i + 1 < argc) {
            unsigned long value; (void)number(argv[++i], 1, MAX_THREADS, &value); config.threads = (size_t)value;
        } else if ((!strcmp(argv[i], "-c") || !strcmp(argv[i], "--config")) && i + 1 < argc) i++;
    }
    struct stat root_info;
    if (stat(config.document_root, &root_info) || !S_ISDIR(root_info.st_mode)) {
        fprintf(stderr, "document_root is not a directory: %s\n", config.document_root); return EXIT_FAILURE;
    }
    if (sqlite3_open(config.database_path, &database) != SQLITE_OK ||
        sqlite3_exec(database, "PRAGMA journal_mode=WAL; CREATE TABLE IF NOT EXISTS notes("
                     "id INTEGER PRIMARY KEY, text TEXT NOT NULL, created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP);"
                     "CREATE TABLE IF NOT EXISTS users("
                     "id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL, email TEXT NOT NULL, created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP);",
                     NULL, NULL, NULL) != SQLITE_OK) {
        fprintf(stderr, "database: %s\n", database ? sqlite3_errmsg(database) : "could not open");
        if (database) sqlite3_close(database);
        return EXIT_FAILURE;
    }
    access_log = !strcmp(config.log_file, "-") ? stdout : fopen(config.log_file, "a");
    if (!access_log) { perror(config.log_file); return EXIT_FAILURE; }
    if (!queue_init()) { perror("queue_init"); return EXIT_FAILURE; }
    started_at = time(NULL);
    struct sigaction action = {0}; action.sa_handler = on_signal; sigemptyset(&action.sa_mask);
    (void)sigaction(SIGINT, &action, NULL); (void)sigaction(SIGTERM, &action, NULL);
    signal(SIGPIPE, SIG_IGN);
    pthread_t *workers = calloc(config.threads, sizeof(*workers));
    if (!workers) { perror("calloc"); return EXIT_FAILURE; }
    size_t started = 0;
    for (; started < config.threads; started++) {
        int error = pthread_create(&workers[started], NULL, worker_main, NULL);
        if (error) { fprintf(stderr, "pthread_create: %s\n", strerror(error)); break; }
    }
    if (started != config.threads) { stop_workers(); }
    else listener = create_listener();
    if (listener < 0 && started == config.threads) { perror("listen"); stop_workers(); }
    if (listener >= 0) {
        fprintf(stderr, "Listening on 0.0.0.0:%u (%zu workers, root %s)\n",
                config.port, config.threads, config.document_root);
        while (!stopping) {
            int fd = accept((int)listener, NULL, NULL);
            if (fd < 0) { if (errno == EINTR) continue; if (stopping) break; perror("accept"); continue; }
            if (!enqueue(fd)) {
                (void)send_text(fd, 503, "Service Unavailable", "text/plain; charset=utf-8",
                                "Server is busy.\n", false, false, "Retry-After: 1");
                close(fd);
            }
        }
        if (listener >= 0) close((int)listener);
        listener = -1; stop_workers();
    }
    for (size_t i = 0; i < started; i++) pthread_join(workers[i], NULL);
    free(workers); free(queue.items);
    pthread_cond_destroy(&queue.not_empty); pthread_mutex_destroy(&queue.mutex);
    if (access_log != stdout) fclose(access_log);
    sqlite3_close(database);
    return started == config.threads ? EXIT_SUCCESS : EXIT_FAILURE;
}
