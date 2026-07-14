#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int write_all(int fd, const char *p, size_t n) {
    while (n) {
        ssize_t k = send(fd, p, n, 0);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return -1;
        p += k; n -= (size_t)k;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 4 || argc > 5) {
        fprintf(stderr, "Usage: %s HOST PORT PATH [METHOD]\n", argv[0]); return 2;
    }
    struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM}, *list;
    int error = getaddrinfo(argv[1], argv[2], &hints, &list);
    if (error) { fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(error)); return 1; }
    int fd = -1;
    for (struct addrinfo *p = list; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd >= 0 && connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
        if (fd >= 0) close(fd);
        fd = -1;
    }
    freeaddrinfo(list);
    if (fd < 0) { perror("connect"); return 1; }
    const char *method = argc == 5 ? argv[4] : "GET";
    char request[8192];
    int n = snprintf(request, sizeof(request), "%s %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
                     method, argv[3], argv[1]);
    if (n < 0 || (size_t)n >= sizeof(request) || write_all(fd, request, (size_t)n)) {
        perror("send"); close(fd); return 1;
    }
    char buffer[8192]; ssize_t got;
    while ((got = recv(fd, buffer, sizeof(buffer), 0)) > 0)
        if (fwrite(buffer, 1, (size_t)got, stdout) != (size_t)got) break;
    close(fd);
    return got < 0 ? 1 : 0;
}
