CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror
LDFLAGS ?=
LDLIBS ?= -pthread

.PHONY: all clean test benchmark

all: http_server

http_server: server.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

test: http_server
	python3 tests/test_server.py ./http_server

benchmark: http_server
	sh ./scripts/benchmark.sh

clean:
	rm -f http_server
