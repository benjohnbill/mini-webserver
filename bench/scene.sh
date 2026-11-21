#!/usr/bin/env bash
# Usage: bench/scene.sh <S2|S2b|S3> <iter|fork|pool|epoll>
# Starts ./server 18080 www --model <model>, runs one Scene, stops the
# server. A Scene measures. It does not pass or fail.
set -u
cd "$(dirname "$0")/.." || exit 1

SCENE=${1:?usage: bench/scene.sh <S2|S2b|S3> <model>}
MODEL=${2:?usage: bench/scene.sh <S2|S2b|S3> <model>}
SERVER=${SERVER:-./server}
HOST=127.0.0.1
PORT=${PORT:-18080}
BASE="http://$HOST:$PORT"
STUCK_COUNT=8
WORK=$(mktemp -d)
SERVER_PID=
# Each client runs in its own process group, so cleanup also stops its sleep.
STUCK_PIDS=()

cleanup() {
    for pid in "${STUCK_PIDS[@]}"; do
        kill -- "-$pid" 2>/dev/null
    done
    if [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill -TERM "$SERVER_PID"
        wait "$SERVER_PID" 2>/dev/null
    fi
    rm -rf "$WORK"
}
trap cleanup EXIT

server_failed() {
    echo "server: $1"
    wait "$SERVER_PID" 2>/dev/null
    echo "--- server stderr ---"
    cat "$WORK/stderr"
    exit 1
}

start_server() {
    [ -x "$SERVER" ] || { echo "$SERVER is not built (run make)"; exit 1; }
    "$SERVER" "$PORT" www --model "$MODEL" >"$WORK/stdout" 2>"$WORK/stderr" &
    SERVER_PID=$!
    for _ in $(seq 50); do
        kill -0 "$SERVER_PID" 2>/dev/null || server_failed "exited at start"
        nc -z "$HOST" "$PORT" 2>/dev/null && break
        sleep 0.1
    done
    sleep 0.3
    kill -0 "$SERVER_PID" 2>/dev/null || server_failed "exited at start"
}

# Each Stuck connection sends a request line and then nothing.
open_stuck_connections() {
    for _ in $(seq "$STUCK_COUNT"); do
        setsid bash -c "(printf 'GET / HTTP/1.1\r\n'; sleep 120) |
            nc $HOST $PORT" >/dev/null 2>&1 &
        STUCK_PIDS+=($!)
    done
}

send_slowly() {
    local request=$'GET / HTTP/1.1\r\nHost: a\r\n\r\n'
    for ((i = 0; i < ${#request}; i++)); do
        printf '%s' "${request:i:1}"
        sleep 3
    done
    sleep 120
}

# Each slow sender sends one byte of a request every 3 s.
open_slow_senders() {
    for _ in $(seq "$STUCK_COUNT"); do
        setsid bash -c "$(declare -f send_slowly); send_slowly |
            nc $HOST $PORT" >/dev/null 2>&1 &
        STUCK_PIDS+=($!)
    done
}

measure_one_curl() {
    sleep 0.5
    echo "curl time_total (s):"
    curl -s -o /dev/null --max-time 120 -w '%{time_total} %{http_code}\n' \
        "$BASE/index.html"
}

run_s3() {
    for connections in 10 100 1000; do
        echo "wrk -t4 -c$connections -d10s --timeout 30s $BASE/index.html"
        wrk -t4 -c"$connections" -d10s --timeout 30s "$BASE/index.html" |
            grep -E 'Requests/sec|Socket errors|requests in'
        kill -0 "$SERVER_PID" 2>/dev/null || server_failed "exited"
    done
}

echo "scene $SCENE, model $MODEL, commit $(git rev-parse --short HEAD 2>/dev/null || echo none)"
start_server
case "$SCENE" in
S2)
    open_stuck_connections
    measure_one_curl
    ;;
S2b)
    open_slow_senders
    measure_one_curl
    ;;
S3)
    run_s3
    ;;
*)
    echo "unknown scene: $SCENE"
    exit 2
    ;;
esac
