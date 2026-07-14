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
        } else if (!strcasecmp(cursor, "Transfer-Encoding")) transfer_encoding = true;
        cursor = end + 2;
    }
    if (request->minor == 1 && !host) return 400;
    if (transfer_encoding) return 501;
    if (strcmp(request->method, "GET") && strcmp(request->method, "HEAD") &&
        strcmp(request->method, "POST")) return 405;
    if (!strcmp(request->method, "POST") && !content_length) return 411;
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
    if (!strcmp(request->target, "/notes") || !strncmp(request->target, "/notes?", 7))
        return notes_route(fd, request, keep_alive, bytes);
    if (!strcmp(request->target, "/health")) {
        const char *body = "{\"status\":\"ok\"}\n"; *bytes = (off_t)strlen(body);
        return send_text(fd, 200, "OK", "application/json", body, request->head_only,
                         keep_alive, NULL) ? 200 : -1;
    }
    if (!strncmp(request->target, "/api/stats", 10) &&
        (request->target[10] == '\0' || request->target[10] == '?')) {
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
    if (!strcmp(request->method, "POST")) {
        const char *body = "No POST route matched.\n"; *bytes = (off_t)strlen(body);
        return send_text(fd, 405, "Method Not Allowed", "text/plain; charset=utf-8", body,
                         false, keep_alive, "Allow: GET, HEAD") ? 405 : -1;
    }
    char relative[MAX_TARGET_BYTES];
    if (!safe_path(request->target, relative, sizeof(relative))) {
        const char *body = "Bad request.\n"; *bytes = (off_t)strlen(body);
        return send_text(fd, 400, "Bad Request", "text/plain; charset=utf-8", body,
                         request->head_only, keep_alive, NULL) ? 400 : -1;
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
            const char *extra = error == 405 ? "Allow: GET, HEAD, POST" : NULL;
            const char *body = error == 405 ? "Only GET, HEAD, and POST are supported.\n" : "Malformed HTTP request.\n";
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
                     "id INTEGER PRIMARY KEY, text TEXT NOT NULL, created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP);",
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
