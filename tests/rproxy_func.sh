#!/usr/bin/env bash
# Usage: tests/rproxy_func.sh
# Runs ./rproxy in front of fake Origins (socat) and in front of ./server,
# sends curl requests over TLS, and exits 1 if one check fails. Checks
# marked "(owner slot)" pass only after the owner types SPEC 11 rows 2, 3.
# Needs: make rproxy cert server, socat, curl, nc.
set -u
cd "$(dirname "$0")/.." || exit 1

RPROXY=${RPROXY:-./rproxy}
HOST=127.0.0.1
PROXY_PORT=18443
SERVER_PORT=18081
ORIGIN_A=18091
ORIGIN_B=18092
ORIGIN_SILENT=18093
ORIGIN_DEAD=18099
BASE="https://localhost:$PROXY_PORT"
WORK=$(mktemp -d)
PIDS=()
PROXY_PID=
FAILS=0

cleanup() {
    stop_proxy
    for pid in "${PIDS[@]}"; do
        kill "$pid" 2>/dev/null
        wait "$pid" 2>/dev/null
    done
    rm -rf "$WORK"
}
trap cleanup EXIT

fail() {
    echo "FAIL $1"
    FAILS=$((FAILS + 1))
}

pass() {
    echo "ok   $1"
}

wait_port() {
    for _ in $(seq 50); do
        nc -z "$HOST" "$1" 2>/dev/null && return 0
        sleep 0.1
    done
    echo "FAIL: nothing listens on port $1"
    exit 1
}

# A fake Origin: it logs the request header and answers with its name.
cat > "$WORK/origin.sh" <<'ORIGIN'
#!/usr/bin/env bash
sed -u $'/^\r$/q' >> "$1"
printf 'HTTP/1.1 200 OK\r\nContent-Length: %s\r\nConnection: close\r\n\r\n%s' \
    "${#2}" "$2"
ORIGIN
chmod +x "$WORK/origin.sh"

start_origin() {
    local name=$1 port=$2
    : > "$WORK/$name.log"
    socat "TCP-LISTEN:$port,reuseaddr,fork" \
        EXEC:"$WORK/origin.sh $WORK/$name.log $name" 2>/dev/null &
    PIDS+=($!)
    wait_port "$port"
}

# An Origin that accepts and never answers.
start_silent_origin() {
    socat "TCP-LISTEN:$ORIGIN_SILENT,reuseaddr,fork" EXEC:'sleep 30' \
        2>/dev/null &
    PIDS+=($!)
    wait_port "$ORIGIN_SILENT"
}

start_proxy() {
    stop_proxy
    "$RPROXY" --port "$PROXY_PORT" --cert build/cert.pem --key build/key.pem \
        --origin-timeout 1 "$@" 2> "$WORK/rproxy.err" &
    PROXY_PID=$!
    wait_port "$PROXY_PORT"
}

stop_proxy() {
    if [ -n "$PROXY_PID" ] && kill -0 "$PROXY_PID" 2>/dev/null; then
        kill -TERM "$PROXY_PID"
        wait "$PROXY_PID" 2>/dev/null
    fi
    PROXY_PID=
}

# Prints "STATUS BODY" for one request.
fetch() {
    curl -sk --max-time 5 -o "$WORK/body" -w '%{http_code}' "$@"
    printf ' %s' "$(cat "$WORK/body" 2>/dev/null)"
}

for tool in socat curl nc; do
    command -v "$tool" > /dev/null || { echo "FAIL: $tool is missing"; exit 1; }
done
for file in "$RPROXY" ./server build/cert.pem build/key.pem; do
    [ -e "$file" ] || { echo "FAIL: $file is missing (make rproxy cert server)"; exit 1; }
done

start_origin A "$ORIGIN_A"
start_origin B "$ORIGIN_B"
start_silent_origin

# Round-Robin over two Origins.
start_proxy --origin "$HOST:$ORIGIN_A" --origin "$HOST:$ORIGIN_B"
got=""
for _ in 1 2 3 4; do
    got="$got$(fetch "$BASE/" | cut -d' ' -f2)"
done
[ "$got" = "ABAB" ] && pass "two Origins get the requests in turn" \
    || fail "two Origins get the requests in turn: got '$got', expected ABAB"

# The request that reaches the Origin.
: > "$WORK/A.log"
fetch -H 'Connection: keep-alive' -H 'Keep-Alive: timeout=5' \
    -A 'probe/1.0' "$BASE/headers" > /dev/null
fetch "$BASE/headers" > /dev/null
log="$WORK/A.log"
grep -q $'^GET /headers HTTP/1.1\r$' "$log" \
    && pass "the Origin gets the request line" \
    || fail "the Origin gets the request line"
grep -q $'^X-Forwarded-For: 127.0.0.1\r$' "$log" \
    && pass "the Origin gets X-Forwarded-For" \
    || fail "the Origin gets X-Forwarded-For"
grep -q '^X-JA4: ' "$log" \
    && pass "the Origin gets X-JA4" || fail "the Origin gets X-JA4"
grep -q $'^User-Agent: probe/1.0\r$' "$log" \
    && pass "the Origin gets the User-Agent" \
    || fail "the Origin gets the User-Agent"
grep -q $'^Connection: close\r$' "$log" \
    && ! grep -qi '^keep-alive:' "$log" \
    && ! grep -qi $'^connection: keep-alive\r$' "$log" \
    && pass "no hop-by-hop header of the client reaches the Origin" \
    || fail "no hop-by-hop header of the client reaches the Origin"

# A dead Origin is skipped once.
start_proxy --origin "$HOST:$ORIGIN_DEAD" --origin "$HOST:$ORIGIN_B"
got="$(fetch "$BASE/")"
[ "$got" = "200 B" ] && pass "a dead Origin is skipped" \
    || fail "a dead Origin is skipped: got '$got'"

# No Origin accepts.
start_proxy --origin "$HOST:$ORIGIN_DEAD"
got="$(fetch "$BASE/" | cut -d' ' -f1)"
[ "$got" = "502" ] && pass "no Origin gives 502" \
    || fail "no Origin gives 502: got '$got'"

# An Origin that never answers.
start_proxy --origin "$HOST:$ORIGIN_SILENT"
start=$(date +%s)
got="$(fetch "$BASE/" | cut -d' ' -f1)"
took=$(($(date +%s) - start))
[ "$got" = "504" ] && [ "$took" -le 3 ] \
    && pass "a silent Origin gives 504 after --origin-timeout" \
    || fail "a silent Origin gives 504 after --origin-timeout: got '$got' in ${took}s"

# Plain HTTP to the TLS port: rproxy closes it and keeps serving.
start_proxy --origin "$HOST:$ORIGIN_A"
printf 'GET / HTTP/1.1\r\nHost: a\r\n\r\n' \
    | timeout 3 nc "$HOST" "$PROXY_PORT" > "$WORK/plain" 2>/dev/null
got="$(fetch "$BASE/")"
[ "$got" = "200 A" ] && kill -0 "$PROXY_PID" 2>/dev/null \
    && pass "plain HTTP is closed and rproxy keeps serving" \
    || fail "plain HTTP is closed and rproxy keeps serving: got '$got'"

# A header over the limit of the Shared core.
long=$(head -c 9000 /dev/zero | tr '\0' 'a')
got="$(fetch -H "X-Long: $long" "$BASE/" | cut -d' ' -f1)"
[ "$got" = "431" ] && pass "a header over 8192 bytes gives 431" \
    || fail "a header over 8192 bytes gives 431: got '$got'"

# The cache: a hit does not reach the Origin.
start_proxy --origin "$HOST:$ORIGIN_A" --cache on
: > "$WORK/A.log"
curl -sk --max-time 5 -D "$WORK/h1" -o /dev/null "$BASE/cached"
curl -sk --max-time 5 -D "$WORK/h2" -o "$WORK/body" "$BASE/cached"
count=$(grep -c '^GET /cached ' "$WORK/A.log")
[ "$count" = "1" ] && grep -qi $'^x-cache: hit\r$' "$WORK/h2" \
    && [ "$(cat "$WORK/body")" = "A" ] \
    && pass "a second GET is a cache hit and does not reach the Origin" \
    || fail "a second GET is a cache hit: Origin saw $count requests"
grep -qi $'^x-cache: miss\r$' "$WORK/h1" \
    && pass "the first GET is a cache miss" \
    || fail "the first GET is a cache miss"
: > "$WORK/A.log"
curl -sk --max-time 5 -o /dev/null --data-binary 'x' "$BASE/cached"
curl -sk --max-time 5 -o /dev/null --data-binary 'x' "$BASE/cached"
count=$(grep -c '^POST /cached ' "$WORK/A.log")
[ "$count" = "2" ] && pass "a POST is not cached" \
    || fail "a POST is not cached: Origin saw $count requests"
start_proxy --origin "$HOST:$ORIGIN_A"
: > "$WORK/A.log"
fetch "$BASE/off" > /dev/null
fetch "$BASE/off" > /dev/null
count=$(grep -c '^GET /off ' "$WORK/A.log")
[ "$count" = "2" ] && pass "without --cache on every GET reaches the Origin" \
    || fail "without --cache on every GET reaches the Origin: saw $count"

# The JA4 as the rate limit key.
start_proxy --origin "$HOST:$ORIGIN_A" --rate 5 --rate-key ja4
got="$(fetch "$BASE/")"
[ "$got" = "200 A" ] && pass "rproxy starts and serves with --rate-key ja4" \
    || fail "rproxy starts and serves with --rate-key ja4: got '$got'"

# The real server as the Origin.
./server "$SERVER_PORT" www > /dev/null 2> "$WORK/server.err" &
PIDS+=($!)
wait_port "$SERVER_PORT"
start_proxy --origin "$HOST:$SERVER_PORT"
curl -sk --max-time 5 -o "$WORK/index" "$BASE/index.html"
cmp -s "$WORK/index" www/index.html \
    && pass "a static file comes through from ./server" \
    || fail "a static file comes through from ./server"
got="$(curl -sk --max-time 5 -H 'Content-Type: text/plain' \
    --data-binary 'hello' "$BASE/echo")"
[ "$got" = "hello" ] && pass "POST /echo comes through from ./server" \
    || fail "POST /echo comes through from ./server: got '$got'"
start_proxy --origin "$HOST:$SERVER_PORT" --cache on
curl -sk --max-time 5 -o /dev/null "$BASE/api/time"
curl -sk --max-time 5 -D "$WORK/time2" -o /dev/null "$BASE/api/time"
grep -qi $'^x-cache: miss\r$' "$WORK/time2" \
    && pass "a no-store reply of ./server is not cached" \
    || fail "a no-store reply of ./server is not cached"

# ETag and 304: the cache answers If-None-Match.
etag="$(curl -sk --max-time 5 -D - -o /dev/null "$BASE/style.css" |
    grep -i '^etag:' | cut -d' ' -f2- | tr -d '\r')"
[ -n "$etag" ] && pass "a static file of ./server has an ETag" \
    || fail "a static file of ./server has an ETag"
curl -sk --max-time 5 -D "$WORK/h304" -o "$WORK/b304" \
    -H "If-None-Match: $etag" "$BASE/style.css"
head -n1 "$WORK/h304" | grep -q '^HTTP/1.1 304 ' \
    && grep -qi $'^x-cache: hit\r$' "$WORK/h304" \
    && grep -qiF "etag: $etag" "$WORK/h304" \
    && [ ! -s "$WORK/b304" ] \
    && pass "a hit with the same If-None-Match is a 304 with no body" \
    || fail "a hit with the same If-None-Match is a 304 with no body"
got="$(curl -sk --max-time 5 -o "$WORK/b200" -w '%{http_code}' \
    -H 'If-None-Match: "other"' "$BASE/style.css")"
[ "$got" = "200" ] && cmp -s "$WORK/b200" www/style.css \
    && pass "a different If-None-Match gets the full 200" \
    || fail "a different If-None-Match gets the full 200: got '$got'"
start_proxy --origin "$HOST:$SERVER_PORT" --cache on
got="$(curl -sk --max-time 5 -o /dev/null -w '%{http_code}' \
    -H "If-None-Match: $etag" "$BASE/style.css")"
[ "$got" = "304" ] && pass "a miss with the same If-None-Match is a 304 too" \
    || fail "a miss with the same If-None-Match is a 304 too: got '$got'"

if [ "$FAILS" -gt 0 ]; then
    echo "--- rproxy stderr ---"
    cat "$WORK/rproxy.err"
    echo "$FAILS failed"
    exit 1
fi
echo "rproxy_func: ok"
