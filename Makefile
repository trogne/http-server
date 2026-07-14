CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror
LDFLAGS ?=
LDLIBS ?= -pthread -lsqlite3

.PHONY: all clean test benchmark

all: http_server tcp_client

http_server: server.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

tcp_client: client.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

test: http_server
	python3 tests/test_server.py ./http_server

benchmark: http_server
	sh ./scripts/benchmark.sh

clean:
	rm -f http_server tcp_client
