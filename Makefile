CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror
LDFLAGS ?=
LDLIBS ?= -pthread

.PHONY: all clean test

all: http_server

http_server: server.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

test: http_server
	python3 tests/test_server.py ./http_server

clean:
	rm -f http_server
