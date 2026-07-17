CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror
LDFLAGS ?=
PG_CONFIG := $(shell command -v pg_config 2>/dev/null)
ifneq ($(PG_CONFIG),)
CFLAGS += -DHAVE_LIBPQ -I$(shell $(PG_CONFIG) --includedir)
LDLIBS ?= -pthread -lsqlite3 -lpq
else
LDLIBS ?= -pthread -lsqlite3
endif

.PHONY: all clean test benchmark

all: http_server tcp_client

http_server: server.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

tcp_client: client.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

test: http_server
	python3 tests/test_frontend.py
	python3 tests/test_server.py ./http_server

benchmark: http_server
	sh ./scripts/benchmark.sh

clean:
	rm -f http_server tcp_client
