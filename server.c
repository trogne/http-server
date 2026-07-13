#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#define DEFAULT_PORT 8080
#define DEFAULT_THREADS 4
#define QUEUE_CAPACITY 256
#define REQUEST_CAPACITY 8192

typedef struct {
    int sockets[QUEUE_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
    bool stopping;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
} connection_queue;

static connection_queue queue = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .not_empty = PTHREAD_COND_INITIALIZER,
};
static volatile sig_atomic_t stop_requested = 0;
static volatile sig_atomic_t listen_fd = -1;

static void handle_signal(int signal_number) {
    (void)signal_number;
    stop_requested = 1;
    if (listen_fd >= 0) {
        close((int)listen_fd);
        listen_fd = -1;
    }
}

static bool parse_positive_number(const char *text, long min, long max, long *value) {
    char *end = NULL;
    errno = 0;
    long parsed = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed < min || parsed > max) {
        return false;
    }
    *value = parsed;
    return true;
}

static bool enqueue_connection(int client_fd) {
    bool accepted = false;
    pthread_mutex_lock(&queue.mutex);
    if (!queue.stopping && queue.count < QUEUE_CAPACITY) {
        queue.sockets[queue.tail] = client_fd;
        queue.tail = (queue.tail + 1) % QUEUE_CAPACITY;
        queue.count++;
        accepted = true;
        pthread_cond_signal(&queue.not_empty);
    }
    pthread_mutex_unlock(&queue.mutex);
    return accepted;
}

static int dequeue_connection(void) {
    pthread_mutex_lock(&queue.mutex);
    while (queue.count == 0 && !queue.stopping) {
        pthread_cond_wait(&queue.not_empty, &queue.mutex);
    }
    if (queue.count == 0 && queue.stopping) {
        pthread_mutex_unlock(&queue.mutex);
        return -1;
    }
    int client_fd = queue.sockets[queue.head];
    queue.head = (queue.head + 1) % QUEUE_CAPACITY;
    queue.count--;
    pthread_mutex_unlock(&queue.mutex);
    return client_fd;
}

static void stop_queue(void) {
    pthread_mutex_lock(&queue.mutex);
    queue.stopping = true;
    pthread_cond_broadcast(&queue.not_empty);
    pthread_mutex_unlock(&queue.mutex);
}

static bool send_all(int fd, const char *data, size_t length) {
    while (length > 0) {
        ssize_t sent = send(fd, data, length, MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        data += (size_t)sent;
        length -= (size_t)sent;
    }
    return true;
}

static void send_response(int fd, int status, const char *reason,
                          const char *content_type, const char *body, bool head_only) {
    char headers[512];
    size_t body_length = strlen(body);
    int header_length = snprintf(headers, sizeof(headers),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "Server: pthread-http/1.0\r\n"
        "\r\n",
        status, reason, content_type, body_length);

    if (header_length <= 0 || (size_t)header_length >= sizeof(headers)) {
        return;
    }
    if (send_all(fd, headers, (size_t)header_length) && !head_only) {
        (void)send_all(fd, body, body_length);
    }
}

static ssize_t read_request_headers(int fd, char *buffer, size_t capacity) {
    size_t used = 0;
    while (used + 1 < capacity) {
        ssize_t received = recv(fd, buffer + used, capacity - used - 1, 0);
        if (received == 0) {
            break;
        }
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        used += (size_t)received;
        buffer[used] = '\0';
        if (strstr(buffer, "\r\n\r\n") != NULL) {
            return (ssize_t)used;
        }
    }
    return (ssize_t)used;
}

static void handle_connection(int client_fd) {
    struct timeval timeout = {.tv_sec = 5, .tv_usec = 0};
    (void)setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void)setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    char request[REQUEST_CAPACITY];
    ssize_t received = read_request_headers(client_fd, request, sizeof(request));
    if (received <= 0) {
        return;
    }
    request[received] = '\0';
    if (strstr(request, "\r\n\r\n") == NULL) {
        send_response(client_fd, 431, "Request Header Fields Too Large",
                      "text/plain; charset=utf-8", "Request headers are too large.\n", false);
        return;
    }

    char method[16];
    char target[2048];
    char version[16];
    if (sscanf(request, "%15s %2047s %15s", method, target, version) != 3 ||
        (strcmp(version, "HTTP/1.0") != 0 && strcmp(version, "HTTP/1.1") != 0)) {
        send_response(client_fd, 400, "Bad Request", "text/plain; charset=utf-8",
                      "Malformed HTTP request.\n", false);
        return;
    }

    bool head_only = strcmp(method, "HEAD") == 0;
    if (strcmp(method, "GET") != 0 && !head_only) {
        send_response(client_fd, 405, "Method Not Allowed", "text/plain; charset=utf-8",
                      "Only GET and HEAD are supported.\n", false);
        return;
    }

    if (strcmp(target, "/") == 0) {
        const char *body =
            "<!doctype html><html><head><meta charset=\"utf-8\">"
            "<title>pthread HTTP server</title></head><body>"
            "<h1>It works!</h1><p>Served by a pthread worker pool on Linux.</p>"
            "</body></html>\n";
        send_response(client_fd, 200, "OK", "text/html; charset=utf-8", body, head_only);
    } else if (strcmp(target, "/health") == 0) {
        send_response(client_fd, 200, "OK", "application/json", "{\"status\":\"ok\"}\n", head_only);
    } else {
        send_response(client_fd, 404, "Not Found", "text/plain; charset=utf-8",
                      "Not found.\n", head_only);
    }
}

static void *worker_main(void *unused) {
    (void)unused;
    for (;;) {
        int client_fd = dequeue_connection();
        if (client_fd < 0) {
            break;
        }
        handle_connection(client_fd);
        close(client_fd);
    }
    return NULL;
}

static int create_listener(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    int enabled = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) < 0) {
        perror("setsockopt");
        close(fd);
        return -1;
    }

    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }
    if (listen(fd, 128) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }
    return fd;
}

int main(int argc, char **argv) {
    long port = DEFAULT_PORT;
    long thread_count = DEFAULT_THREADS;
    if (argc > 3 || (argc >= 2 && !parse_positive_number(argv[1], 1, 65535, &port)) ||
        (argc == 3 && !parse_positive_number(argv[2], 1, 256, &thread_count))) {
        fprintf(stderr, "Usage: %s [port 1-65535] [threads 1-256]\n", argv[0]);
        return EXIT_FAILURE;
    }

    struct sigaction action = {0};
    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) < 0 || sigaction(SIGTERM, &action, NULL) < 0) {
        perror("sigaction");
        return EXIT_FAILURE;
    }
    signal(SIGPIPE, SIG_IGN);

    sigset_t shutdown_signals;
    sigemptyset(&shutdown_signals);
    sigaddset(&shutdown_signals, SIGINT);
    sigaddset(&shutdown_signals, SIGTERM);
    int mask_error = pthread_sigmask(SIG_BLOCK, &shutdown_signals, NULL);
    if (mask_error != 0) {
        fprintf(stderr, "pthread_sigmask: %s\n", strerror(mask_error));
        return EXIT_FAILURE;
    }

    pthread_t *workers = calloc((size_t)thread_count, sizeof(*workers));
    if (workers == NULL) {
        perror("calloc");
        return EXIT_FAILURE;
    }

    long workers_started = 0;
    for (; workers_started < thread_count; workers_started++) {
        int error = pthread_create(&workers[workers_started], NULL, worker_main, NULL);
        if (error != 0) {
            fprintf(stderr, "pthread_create: %s\n", strerror(error));
            stop_queue();
            for (long i = 0; i < workers_started; i++) {
                pthread_join(workers[i], NULL);
            }
            free(workers);
            return EXIT_FAILURE;
        }
    }

    listen_fd = create_listener((uint16_t)port);
    if (listen_fd < 0) {
        stop_queue();
        for (long i = 0; i < workers_started; i++) {
            pthread_join(workers[i], NULL);
        }
        free(workers);
        return EXIT_FAILURE;
    }

    printf("Listening on http://0.0.0.0:%ld with %ld worker threads\n", port, thread_count);
    fflush(stdout);

    mask_error = pthread_sigmask(SIG_UNBLOCK, &shutdown_signals, NULL);
    if (mask_error != 0) {
        fprintf(stderr, "pthread_sigmask: %s\n", strerror(mask_error));
        close((int)listen_fd);
        listen_fd = -1;
        stop_queue();
        for (long i = 0; i < workers_started; i++) {
            pthread_join(workers[i], NULL);
        }
        free(workers);
        return EXIT_FAILURE;
    }

    while (!stop_requested) {
        int client_fd = accept((int)listen_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (stop_requested || errno == EBADF || errno == EINVAL) {
                break;
            }
            perror("accept");
            continue;
        }
        if (!enqueue_connection(client_fd)) {
            send_response(client_fd, 503, "Service Unavailable", "text/plain; charset=utf-8",
                          "Server is busy.\n", false);
            close(client_fd);
        }
    }

    if (listen_fd >= 0) {
        close((int)listen_fd);
        listen_fd = -1;
    }
    stop_queue();
    for (long i = 0; i < workers_started; i++) {
        pthread_join(workers[i], NULL);
    }
    free(workers);
    pthread_cond_destroy(&queue.not_empty);
    pthread_mutex_destroy(&queue.mutex);
    puts("Server stopped.");
    return EXIT_SUCCESS;
}
