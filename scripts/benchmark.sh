#!/bin/sh
set -eu

command -v wrk >/dev/null 2>&1 || {
    echo "wrk is required (Debian/Ubuntu: sudo apt install wrk)" >&2
    exit 1
}

PORT="${PORT:-18080}"
CONNECTIONS="${CONNECTIONS:-100}"
SERVER_THREADS="${SERVER_THREADS:-$CONNECTIONS}"
WRK_THREADS="${WRK_THREADS:-4}"
DURATION="${DURATION:-10s}"
CONFIG="$(mktemp)"
cleanup() {
    if [ "${PID:-}" ]; then kill "$PID" 2>/dev/null || true; wait "$PID" 2>/dev/null || true; fi
    rm -f "$CONFIG"
}
trap cleanup EXIT INT TERM

cat >"$CONFIG" <<EOF
port = $PORT
threads = $SERVER_THREADS
queue_capacity = 1024
keepalive_timeout = 10
keepalive_requests = 10000
document_root = ./public
log_file = /dev/null
EOF

./http_server --config "$CONFIG" &
PID=$!
i=0
until curl -fsS "http://127.0.0.1:$PORT/health" >/dev/null 2>&1; do
    i=$((i + 1))
    [ "$i" -lt 50 ] || { echo "server failed to start" >&2; exit 1; }
    sleep 0.1
done

echo "Benchmarking with $SERVER_THREADS server workers, $WRK_THREADS wrk threads, $CONNECTIONS connections for $DURATION"
wrk -t"$WRK_THREADS" -c"$CONNECTIONS" -d"$DURATION" --timeout 5s --latency \
    "http://127.0.0.1:$PORT/index.html"
