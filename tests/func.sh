#!/usr/bin/env bash
# Usage: tests/func.sh <iter|fork|pool|epoll>
# Starts ./server 18080 www --model <model> (SERVER=./server-asan to use the
# ASan build), runs curl and nc checks, and exits 1 at the first failure.
set -u
cd "$(dirname "$0")/.." || exit 1

MODEL=${1:?usage: tests/func.sh <iter|fork|pool|epoll>}
SERVER=${SERVER:-./server}
HOST=127.0.0.1
PORT=18080
BASE="http://$HOST:$PORT"
WORK=$(mktemp -d)
SERVER_PID=

cleanup() {
    if [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill -TERM "$SERVER_PID"
        wait "$SERVER_PID" 2>/dev/null
    fi
    rm -rf "$WORK"
}
trap cleanup EXIT

server_failed() {
    echo "FAIL server: $1"
    wait "$SERVER_PID" 2>/dev/null
    echo "--- server stderr ---"
    cat "$WORK/stderr"
    exit 1
}

start_server() {
    [ -x "$SERVER" ] || { echo "FAIL: $SERVER is not built (run make)"; exit 1; }
    if [ ! -f www/image.png ] || [ ! -f www/large.bin ]; then
        make -s www || exit 1
    fi
    "$SERVER" "$PORT" www --model "$MODEL" >"$WORK/stdout" 2>"$WORK/stderr" &
    SERVER_PID=$!
    for _ in $(seq 50); do
        kill -0 "$SERVER_PID" 2>/dev/null || server_failed "exited at start"
        nc -z "$HOST" "$PORT" 2>/dev/null && break
        sleep 0.1
    done
    # The port can answer before a model returns its error.
    sleep 0.3
    kill -0 "$SERVER_PID" 2>/dev/null || server_failed "exited at start"
    nc -z "$HOST" "$PORT" 2>/dev/null || server_failed "not listening on $PORT"
}

expect() {
    local name=$1 expected=$2 actual=$3
    if [ "$actual" = "$expected" ]; then
        echo "PASS $name"
        return
    fi
    echo "FAIL $name: expected [$expected], got [$actual]"
    kill -0 "$SERVER_PID" 2>/dev/null || server_failed "exited"
    exit 1
}

# curl: prints the status code.
http_status() {
    curl -s -o /dev/null --max-time 10 -w '%{http_code}' "$@"
}

# curl: prints one response header value, without the line end.
http_header() {
    local name=$1
    shift
    curl -s -o /dev/null --max-time 10 -D - "$@" |
        grep -i "^$name:" | head -n1 | cut -d' ' -f2- | tr -d '\r'
}

# nc: sends the bytes of a file and prints the response.
raw_send() {
    timeout 10 nc -N "$HOST" "$PORT" <"$1"
}

raw_status() {
    raw_send "$1" | head -n1 | tr -d '\r' | cut -d' ' -f2
}

raw_header() {
    raw_send "$1" | grep -i "^$2:" | head -n1 | cut -d' ' -f2- | tr -d '\r'
}

# Writes printf-escaped text to a file and prints the file name.
request_file() {
    local file="$WORK/request.$RANDOM$RANDOM"
    printf "$1" >"$file"
    echo "$file"
}

# A GET / whose header section is exactly $1 bytes.
request_with_header_len() {
    local head='GET / HTTP/1.1\r\nHost: a\r\nConnection: close\r\nX-Pad: '
    local file="$WORK/header.$1"
    {
        printf "$head"
        head -c $(($1 - 55)) /dev/zero | tr '\0' p
        printf '\r\n\r\n'
    } >"$file"
    echo "$file"
}

# A target of exactly $1 bytes: "/" and then letters, with a "/" every 200
# bytes. A path part over NAME_MAX (255) gets ENAMETOOLONG, which is 500.
target_of_len() {
    printf '/'
    head -c $(($1 - 1)) /dev/zero | tr '\0' a | sed -E 's/(.{199})./\1\//g'
}

check_static_files() {
    expect "GET / is 200" 200 "$(http_status "$BASE/")"
    expect "GET / is text/html" text/html "$(http_header Content-Type "$BASE/")"
    expect "style.css is text/css" text/css \
        "$(http_header Content-Type "$BASE/style.css")"
    expect "app.js is text/javascript" text/javascript \
        "$(http_header Content-Type "$BASE/app.js")"
    expect "image.png is image/png" image/png \
        "$(http_header Content-Type "$BASE/image.png")"
    expect "large.bin has 10485760 bytes" 10485760 \
        "$(curl -s -o /dev/null --max-time 20 -w '%{size_download}' \
            "$BASE/large.bin")"
    expect "a missing file is 404" 404 "$(http_status "$BASE/missing.html")"
    expect "a .. segment is 403" 403 \
        "$(http_status --path-as-is "$BASE/../etc/passwd")"
}

check_handlers() {
    local body
    body=$(curl -s --max-time 10 "$BASE/api/time")
    [[ $body =~ ^\{\"now\":\"[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9:]{8}Z\"\}$ ]] &&
        body=match
    expect "GET /api/time is the time in JSON" match "$body"
    expect "GET /api/time is application/json" application/json \
        "$(http_header Content-Type "$BASE/api/time")"
    expect "POST /echo sends back the body" hello \
        "$(curl -s --max-time 10 -H 'Content-Type: text/plain' \
            --data-binary hello "$BASE/echo")"
    expect "POST /echo sends back the Content-Type" text/plain \
        "$(http_header Content-Type -H 'Content-Type: text/plain' \
            --data-binary hello "$BASE/echo")"
    expect "GET /echo is 405" 405 "$(http_status "$BASE/echo")"
    expect "GET /echo allows POST" POST "$(http_header Allow "$BASE/echo")"
    expect "POST to a file is 405" 405 \
        "$(http_status --data-binary x "$BASE/index.html")"
    expect "POST to a file allows GET" GET \
        "$(http_header Allow --data-binary x "$BASE/index.html")"
}

check_errors() {
    local file
    file=$(request_file 'GARBAGE\r\n\r\n')
    expect "a request line with no spaces is 400" 400 "$(raw_status "$file")"
    file=$(request_file 'GET index.html HTTP/1.1\r\nHost: a\r\n\r\n')
    expect "a target with no leading / is 400" 400 "$(raw_status "$file")"
    file=$(request_file 'POST /echo HTTP/1.1\r\nHost: a\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\nx')
    expect "two Content-Length headers are 400" 400 "$(raw_status "$file")"
    file=$(request_file 'POST /echo HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n')
    expect "Transfer-Encoding is 400" 400 "$(raw_status "$file")"
    file=$(request_file 'GET / HTTP/2.0\r\nHost: a\r\n\r\n')
    expect "HTTP/2.0 is 505" 505 "$(raw_status "$file")"
    file=$(request_file 'DELETE / HTTP/1.1\r\nHost: a\r\n\r\n')
    expect "DELETE is 405" 405 "$(raw_status "$file")"
    file=$(request_file 'POST /echo HTTP/1.1\r\nHost: a\r\n\r\n')
    expect "POST with no Content-Length is 411" 411 "$(raw_status "$file")"
}

check_limits() {
    local file
    expect "a target of 2048 bytes passes (404)" 404 \
        "$(http_status "$BASE$(target_of_len 2048)")"
    expect "a target of 2049 bytes is 414" 414 \
        "$(http_status "$BASE$(target_of_len 2049)")"
    file=$(request_with_header_len 8192)
    expect "a header of 8192 bytes passes" 200 "$(raw_status "$file")"
    file=$(request_with_header_len 8193)
    expect "a header of 8193 bytes is 431" 431 "$(raw_status "$file")"
    head -c 1048576 /dev/zero >"$WORK/body.max"
    expect "a body of 1048576 bytes passes" 1048576 \
        "$(curl -s -o /dev/null --max-time 20 -w '%{size_download}' \
            -H 'Expect:' --data-binary @"$WORK/body.max" "$BASE/echo")"
    file=$(request_file 'POST /echo HTTP/1.1\r\nHost: a\r\nContent-Length: 1048577\r\n\r\n')
    expect "a body of 1048577 bytes is 413" 413 "$(raw_status "$file")"
}

check_connections() {
    local file
    expect "HTTP/1.1 keeps the connection open" "1 0" \
        "$(curl -s -o /dev/null -o /dev/null --max-time 10 \
            -w '%{num_connects} ' "$BASE/" "$BASE/style.css" | sed 's/ $//')"
    expect "HTTP/1.1 says keep-alive" keep-alive \
        "$(http_header Connection "$BASE/")"
    file=$(request_file 'GET / HTTP/1.0\r\n\r\n')
    expect "HTTP/1.0 closes" close "$(raw_header "$file" Connection)"
    file=$(request_file 'GET / HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n')
    expect "Connection: close closes" close "$(raw_header "$file" Connection)"
    file=$(request_file 'GET / HTTP/1.1\nHost: a\nConnection: close\n\n')
    expect "bare \\n line ends are 200" 200 "$(raw_status "$file")"
}

# The server closes a silent connection after --timeout (default 5 s).
check_timeout() {
    local start elapsed
    start=$(date +%s)
    exec 3<>"/dev/tcp/$HOST/$PORT"
    timeout 10 cat <&3 >/dev/null
    exec 3<&-
    elapsed=$(($(date +%s) - start))
    expect "a silent connection closes in 4 to 7 s" yes \
        "$([ "$elapsed" -ge 4 ] && [ "$elapsed" -le 7 ] && echo yes ||
            echo "no (${elapsed} s)")"
}

start_server
check_static_files
check_handlers
check_errors
check_limits
check_connections
check_timeout
echo "ALL PASS ($MODEL)"
