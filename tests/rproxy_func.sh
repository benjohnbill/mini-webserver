#!/usr/bin/env bash
# Usage: tests/rproxy_func.sh
# Runs ./rproxy in front of fake Origins (socat) and in front of ./server,
# sends curl requests over TLS, and exits 1 if one check fails. Checks
# marked "(owner slot)" pass only after the owner types SPEC 11 rows 2, 3.
# Needs: make rproxy cert server, socat, curl, nc, openssl, sha256sum.
set -u
cd "$(dirname "$0")/.." || exit 1

RPROXY=${RPROXY:-./rproxy}
HOST=127.0.0.1
PROXY_PORT=18443
SERVER_PORT=18081
EPOLL_PORT=18083
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

for tool in socat curl nc openssl sha256sum; do
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

# The connection Verdict: a block list hit gets no TLS handshake.
printf '# test ranges\n127.0.0.0/8\n' > "$WORK/blocked.txt"
start_proxy --origin "$HOST:$ORIGIN_A" --block-list "$WORK/blocked.txt"
: > "$WORK/A.log"
curl -sk --max-time 5 -o /dev/null "$BASE/" 2>/dev/null
code=$?
count=$(grep -c '^GET ' "$WORK/A.log")
{ [ "$code" = "35" ] || [ "$code" = "56" ]; } && [ "$count" = "0" ] \
    && pass "a client IP in the block list gets no TLS handshake" \
    || fail "a client IP in the block list gets no TLS handshake: curl exit $code, Origin saw $count"
grep -q '^127\.0\.0\.1 [^ ]* drop - - - 0$' "$WORK/rproxy.err" \
    && pass "a dropped connection logs the Verdict drop" \
    || fail "a dropped connection logs the Verdict drop"
printf '10.0.0.0/8\n2001:db8::/32\n' > "$WORK/other.txt"
start_proxy --origin "$HOST:$ORIGIN_A" --block-list "$WORK/other.txt"
got="$(fetch "$BASE/")"
[ "$got" = "200 A" ] && pass "a client IP outside the block list passes" \
    || fail "a client IP outside the block list passes: got '$got'"
# Stop the last rproxy, so that a failed bind does not pass as a stop.
stop_proxy
printf '10.0.0.0/33\n' > "$WORK/bad.txt"
"$RPROXY" --port "$PROXY_PORT" --cert build/cert.pem --key build/key.pem \
    --origin "$HOST:$ORIGIN_A" --block-list "$WORK/bad.txt" \
    2> "$WORK/bad.err" &
bad_pid=$!
sleep 0.5
if kill -0 "$bad_pid" 2>/dev/null; then
    kill "$bad_pid"; wait "$bad_pid" 2>/dev/null
    fail "a bad block list stops rproxy at the start"
else
    grep -q 'line 1' "$WORK/bad.err" \
        && pass "a bad block list stops rproxy at the start" \
        || fail "a bad block list stops rproxy at the start: no line number"
fi

# The JA4 table: a label alone does not drop (SPEC 11 row 4).
printf 't13d1516h2_8daaf6152771_e5627efa2ab1 browser\n' > "$WORK/ja4.txt"
start_proxy --origin "$HOST:$ORIGIN_A" --ja4-list "$WORK/ja4.txt"
got="$(fetch "$BASE/")"
[ "$got" = "200 A" ] && pass "rproxy starts and serves with --ja4-list" \
    || fail "rproxy starts and serves with --ja4-list: got '$got'"

# The block table: a key is dropped until its time, then it passes.
start_proxy --origin "$HOST:$ORIGIN_A" --block-key 127.0.0.1 --block-time 2
curl -sk --max-time 5 -o /dev/null "$BASE/" 2>/dev/null
code=$?
[ "$code" = "35" ] || [ "$code" = "56" ] \
    && pass "a client IP in the block table gets no TLS handshake" \
    || fail "a client IP in the block table gets no TLS handshake: curl exit $code"
sleep 2.2
got="$(fetch "$BASE/")"
[ "$got" = "200 A" ] && pass "a block table entry expires after --block-time" \
    || fail "a block table entry expires after --block-time: got '$got'"

# A client over the limit after five 429s in the window goes into the
# block table. --rate 0.01 --burst 1: the 1st request passes, the 2nd to
# the 7th get 429, and the 7th (five 429s before it) blocks the client.
start_proxy --origin "$HOST:$ORIGIN_A" --rate 0.01 --burst 1 --block-time 60
codes=""
for _ in 1 2 3 4 5 6 7; do
    codes="$codes $(curl -sk --max-time 5 -o /dev/null -w '%{http_code}' \
        "$BASE/")"
done
curl -sk --max-time 5 -o /dev/null "$BASE/" 2>/dev/null
code=$?
[ "$codes" = " 200 429 429 429 429 429 429" ] \
    && { [ "$code" = "35" ] || [ "$code" = "56" ]; } \
    && grep -q '^127\.0\.0\.1 [^ ]* drop - - - 0$' "$WORK/rproxy.err" \
    && pass "a client over the limit after five 429s gets no TLS handshake" \
    || fail "a client over the limit after five 429s gets no TLS handshake:" \
        "codes '$codes', curl exit $code"

# Proof of work (SPEC 11 v2, row "Test"). --pow-bits 8: a solution has
# SHA-256 of "NONCE:COUNTER" with 8 leading zero bits, the hex "00".
pow_hash() {
    printf '%s' "$1" | sha256sum | cut -c1-2
}
start_proxy --origin "$HOST:$ORIGIN_A" --pow-prefix /data/ --pow-bits 8
curl -sk --max-time 5 -D "$WORK/hpow" -o /dev/null "$BASE/data/1.html"
challenge="$(grep -i '^x-pow-challenge:' "$WORK/hpow" | cut -d' ' -f2- |
    tr -d '\r')"
nonce="${challenge% *}"
head -n1 "$WORK/hpow" | grep -q '^HTTP/1.1 401 ' \
    && [ "${challenge#* }" = "8" ] && [ "${#nonce}" = "64" ] \
    && grep -qiF "www-authenticate: PoW challenge=\"$challenge\"" "$WORK/hpow" \
    && grep -q '^127\.0\.0\.1 [^ ]* challenge - - 401 ' "$WORK/rproxy.err" \
    && pass "a pow target with no X-PoW gets 401 and X-PoW-Challenge" \
    || fail "a pow target with no X-PoW gets 401 and X-PoW-Challenge: '$challenge'"
counter=0
while [ "$(pow_hash "$nonce:$counter")" != "00" ]; do
    counter=$((counter + 1))
done
got="$(fetch -H "X-PoW: $nonce:$counter" "$BASE/data/1.html")"
[ "$got" = "200 A" ] && pass "a correct X-PoW gets 200" \
    || fail "a correct X-PoW gets 200: got '$got' for $nonce:$counter"
wrong=0
while [ "$(pow_hash "$nonce:$wrong")" = "00" ]; do
    wrong=$((wrong + 1))
done
got="$(fetch -H "X-PoW: $nonce:$wrong" "$BASE/data/1.html" | cut -d' ' -f1)"
[ "$got" = "401" ] && pass "a wrong X-PoW gets 401 again" \
    || fail "a wrong X-PoW gets 401 again: got '$got'"
got="$(fetch "$BASE/index.html")"
[ "$got" = "200 A" ] && pass "a target outside --pow-prefix needs no X-PoW" \
    || fail "a target outside --pow-prefix needs no X-PoW: got '$got'"

# Web Bot Auth and 402 (SPEC 11 v2, row "Deal"). tests/data holds a TEST
# key pair: its only use is this file. The signature covers @authority
# and the signature-agent member agent1 (RFC 9421 section 2.5).
KEYID=C40IKiW5iEzzt2B5DdKbd4pP26xcG8mbp_gDsQ93cnQ
AGENT='"https://crawler.example/.well-known/http-message-signatures-directory"'
GPTBOT='Mozilla/5.0 AppleWebKit/537.36 (KHTML, like Gecko; compatible; GPTBot/1.1; +https://openai.com/gptbot)'
created=$(date +%s)
params="(\"@authority\" \"signature-agent\";key=\"agent1\");created=$created;expires=$((created + 60));keyid=\"$KEYID\";alg=\"ed25519\";tag=\"web-bot-auth\""
printf '"@authority": localhost:%s\n"signature-agent";key="agent1": %s\n"@signature-params": %s' \
    "$PROXY_PORT" "$AGENT" "$params" > "$WORK/base.txt"
signature="$(openssl pkeyutl -sign -rawin -inkey tests/data/bot_test_key.pem \
    -in "$WORK/base.txt" | base64 -w0)"
broken="$(printf '%s' "$signature" | tr 'A-Za-z' 'B-ZAb-za')"
signed_fetch() {
    fetch -A "$GPTBOT" -H "Signature-Agent: agent1=$AGENT" \
        -H "Signature-Input: sig1=$params" -H "Signature: sig1=:$1:" "$2"
}
start_proxy --origin "$HOST:$ORIGIN_A" --pay-prefix /data/ \
    --bot-key tests/data/bot_test_keys.txt
curl -sk --max-time 5 -D "$WORK/hpay" -o /dev/null -A "$GPTBOT" \
    "$BASE/data/1.html"
head -n1 "$WORK/hpay" | grep -q '^HTTP/1.1 402 ' \
    && grep -qi $'^crawler-price: USD 0.01\r$' "$WORK/hpay" \
    && grep -q '^127\.0\.0\.1 [^ ]* pay - - 402 ' "$WORK/rproxy.err" \
    && pass "a crawler User-Agent on a pay target gets 402 and crawler-price" \
    || fail "a crawler User-Agent on a pay target gets 402 and crawler-price"
got="$(fetch -A "$GPTBOT" "$BASE/index.html" | cut -d' ' -f1)"
[ "$got" = "403" ] && pass "a crawler User-Agent outside --pay-prefix is denied" \
    || fail "a crawler User-Agent outside --pay-prefix is denied: got '$got'"
got="$(signed_fetch "$signature" "$BASE/data/1.html")"
[ "$got" = "200 A" ] && pass "a signed crawler passes on a pay target" \
    || fail "a signed crawler passes on a pay target: got '$got'"
got="$(signed_fetch "$signature" "$BASE/index.html")"
[ "$got" = "200 A" ] && pass "a signed crawler is not denied" \
    || fail "a signed crawler is not denied: got '$got'"
got="$(signed_fetch "$broken" "$BASE/data/1.html" | cut -d' ' -f1)"
[ "$got" = "402" ] && pass "a broken signature is not verified" \
    || fail "a broken signature is not verified: got '$got'"
# Stop the last rproxy, so that a failed bind does not pass as a stop.
stop_proxy
printf 'not-a-key-line\n' > "$WORK/bad_keys.txt"
"$RPROXY" --port "$PROXY_PORT" --cert build/cert.pem --key build/key.pem \
    --origin "$HOST:$ORIGIN_A" --bot-key "$WORK/bad_keys.txt" \
    2> "$WORK/bad.err" &
bad_pid=$!
sleep 0.5
if kill -0 "$bad_pid" 2>/dev/null; then
    kill "$bad_pid"; wait "$bad_pid" 2>/dev/null
    fail "a bad --bot-key file stops rproxy at the start"
else
    grep -q 'line 1' "$WORK/bad.err" \
        && pass "a bad --bot-key file stops rproxy at the start" \
        || fail "a bad --bot-key file stops rproxy at the start: no line number"
fi

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

# robots.txt and the Honeypot.
start_proxy --origin "$HOST:$SERVER_PORT" --tarpit 1 --block-time 60
curl -sk --max-time 5 -o "$WORK/robots" "$BASE/robots.txt"
cmp -s "$WORK/robots" www/robots.txt \
    && pass "GET /robots.txt comes through from ./server" \
    || fail "GET /robots.txt comes through from ./server"
got="$(curl -sk --max-time 5 -o /dev/null -w '%{http_code}' "$BASE/trap/")"
[ "$got" = "000" ] \
    && grep -q '^127\.0\.0\.1 [^ ]* tarpit - - - 0$' "$WORK/rproxy.err" \
    && pass "a GET of /trap/ is tarpitted" \
    || fail "a GET of /trap/ is tarpitted: got '$got'"
grep -q '^behavior 127\.0\.0\.1 requests=2 targets=2 not_found=0 robots=1 trap=1 slowed=0$' \
    "$WORK/rproxy.err" \
    && pass "the behavior counters count the robots.txt read and the trap hit" \
    || fail "the behavior counters count the robots.txt read and the trap hit"
curl -sk --max-time 5 -o /dev/null "$BASE/" 2>/dev/null
code=$?
{ [ "$code" = "35" ] || [ "$code" = "56" ]; } \
    && grep -q '^127\.0\.0\.1 [^ ]* drop - - - 0$' "$WORK/rproxy.err" \
    && pass "after the trap the same client IP gets no TLS handshake" \
    || fail "after the trap the same client IP gets no TLS handshake: curl exit $code"
got="$(curl -sk --max-time 5 --interface 127.0.0.2 -o /dev/null \
    -w '%{http_code}' "$BASE/")"
[ "$got" = "200" ] && pass "a client that did not touch the trap still passes" \
    || fail "a client that did not touch the trap still passes: got '$got'"

# Five 404 replies in the window: the next request is denied, and the
# client IP is blocked (fail2ban maxretry 5).
for _ in 1 2 3 4 5; do
    curl -sk --max-time 5 --interface 127.0.0.3 -o /dev/null \
        "$BASE/missing.html"
done
got="$(curl -sk --max-time 5 --interface 127.0.0.3 -o /dev/null \
    -w '%{http_code}' "$BASE/")"
[ "$got" = "403" ] && pass "a client with five 404 replies is denied" \
    || fail "a client with five 404 replies is denied: got '$got'"
curl -sk --max-time 5 --interface 127.0.0.3 -o /dev/null "$BASE/" 2>/dev/null
code=$?
[ "$code" = "35" ] || [ "$code" = "56" ] \
    && pass "after five 404 replies the client IP gets no TLS handshake" \
    || fail "after five 404 replies the client IP gets no TLS handshake: curl exit $code"

# Event loops (docs/adr/reverse-proxy-runs-one-event-loop-per-core.md):
# with one loop, a held connection does not hold the other clients.
start_proxy --origin "$HOST:$SERVER_PORT" --loops 1 --tarpit 3 --block-time 60
curl -sk --max-time 6 --interface 127.0.0.4 -o /dev/null "$BASE/trap/" &
trap_pid=$!
sleep 0.5
got="$(curl -sk --max-time 2 --interface 127.0.0.5 -o /dev/null \
    -w '%{http_code}' "$BASE/")"
[ "$got" = "200" ] \
    && pass "with one Event loop, a tarpit does not hold another client" \
    || fail "with one Event loop, a tarpit does not hold another client: got '$got'"
wait "$trap_pid" 2>/dev/null
grep -q '^127\.0\.0\.4 [^ ]* tarpit - - - 0$' "$WORK/rproxy.err" \
    && pass "the tarpit ends after --tarpit seconds and is logged" \
    || fail "the tarpit ends after --tarpit seconds and is logged"

# A Stuck connection: it sends no ClientHello.
sleep 30 | nc 127.0.0.1 "$PROXY_PORT" > /dev/null 2>&1 &
stuck_pid=$!
sleep 0.5
got="$(curl -sk --max-time 2 --interface 127.0.0.6 -o /dev/null \
    -w '%{http_code}' "$BASE/")"
[ "$got" = "200" ] \
    && pass "with one Event loop, a Stuck connection does not hold another client" \
    || fail "with one Event loop, a Stuck connection does not hold another client: got '$got'"
kill "$stuck_pid" 2>/dev/null; wait "$stuck_pid" 2>/dev/null

# Many clients at once on few loops: each one gets its reply.
start_proxy --origin "$HOST:$SERVER_PORT" --loops 2
# Wait for the curl jobs only: a bare wait also waits for the Origins.
: > "$WORK/many"
many_pids=()
for _ in $(seq 1 40); do
    curl -sk --max-time 5 -o /dev/null -w '%{http_code}\n' "$BASE/" \
        >> "$WORK/many" &
    many_pids+=($!)
done
wait "${many_pids[@]}"
[ "$(grep -c '^200$' "$WORK/many")" = "40" ] \
    && pass "40 clients at once on 2 Event loops all get 200" \
    || fail "40 clients at once on 2 Event loops all get 200: $(sort "$WORK/many" | uniq -c | tr '\n' ' ')"

# A POST body larger than one TLS record reaches the Origin whole.
head -c 40000 /dev/zero | tr '\0' 'a' > "$WORK/big_body"
start_proxy --origin "$HOST:$ORIGIN_A"
: > "$WORK/A.log"
curl -sk --max-time 5 -o /dev/null --data-binary @"$WORK/big_body" "$BASE/echo"
grep -q $'^Content-Length: 40000\r$' "$WORK/A.log" \
    && pass "a 40000-byte POST body reaches the Origin with its length" \
    || fail "a 40000-byte POST body reaches the Origin with its length"

# A second rproxy on a port that one already holds stops at the start.
# SO_REUSEPORT must not let it share the port.
"$RPROXY" --port "$PROXY_PORT" --cert build/cert.pem --key build/key.pem \
    --origin "$HOST:$ORIGIN_A" 2> "$WORK/second.err" &
second_pid=$!
sleep 0.5
if kill -0 "$second_pid" 2>/dev/null; then
    kill "$second_pid"; wait "$second_pid" 2>/dev/null
    fail "a second rproxy on the same port stops at the start"
else
    grep -q 'in use' "$WORK/second.err" \
        && pass "a second rproxy on the same port stops at the start" \
        || fail "a second rproxy on the same port stops at the start: $(cat "$WORK/second.err")"
fi

# A stop closes the open connections, and their log line says stop.
sleep 30 | nc 127.0.0.1 "$PROXY_PORT" > /dev/null 2>&1 &
stuck_pid=$!
sleep 0.5
stop_proxy
kill "$stuck_pid" 2>/dev/null; wait "$stuck_pid" 2>/dev/null
grep -q '^127\.0\.0\.1 - stop - - - 0$' "$WORK/rproxy.err" \
    && pass "a connection that a stop closes logs the Verdict stop" \
    || fail "a connection that a stop closes logs the Verdict stop"

# An Origin name with two addresses: the first (::1) refuses, because the
# fake Origin listens on IPv4 only. rproxy tries the second address. A
# private /etc/hosts needs a user and mount namespace (unshare -rm).
printf '::1 multi.test\n127.0.0.1 multi.test\n' > "$WORK/hosts"
if unshare -rm true 2>/dev/null; then
    stop_proxy
    unshare -rm sh -c 'mount --bind "$1" /etc/hosts && shift && exec "$@"' \
        sh "$WORK/hosts" "$RPROXY" --port "$PROXY_PORT" \
        --cert build/cert.pem --key build/key.pem --origin-timeout 1 \
        --origin "multi.test:$ORIGIN_A" 2> "$WORK/rproxy.err" &
    PROXY_PID=$!
    wait_port "$PROXY_PORT"
    got="$(fetch "$BASE/multi")"
    [ "$got" = "200 A" ] \
        && pass "an Origin name tries its next address when the first refuses" \
        || fail "an Origin name tries its next address when the first refuses: got '$got'"
    stop_proxy
else
    echo "skip an Origin name with two addresses: unshare -rm is not allowed"
fi

# Relay modes (SPEC 12). The Origin is ./server --model epoll, because a
# pool Origin stalls slow readers. The file is larger than ORIGIN_REPLY_MAX.
mkdir -p "$WORK/www"
head -c 6291456 /dev/urandom > "$WORK/www/big.bin"
big_sum=$(sha256sum < "$WORK/www/big.bin" | cut -d' ' -f1)
./server "$EPOLL_PORT" "$WORK/www" --model epoll > /dev/null 2>&1 &
PIDS+=($!)
wait_port "$EPOLL_PORT"
# Prints "STATUS SHA256" of one download of big.bin. $1 is the output file.
fetch_big() {
    local out=$1
    shift
    local status
    status=$(curl -sk --max-time 30 -o "$out" -w '%{http_code}' "$@" \
        "$BASE/big.bin")
    printf '%s %s' "$status" "$(sha256sum < "$out" | cut -d' ' -f1)"
}

start_proxy --origin "$HOST:$EPOLL_PORT"
got="$(fetch_big "$WORK/big_cap" | cut -d' ' -f1)"
[ "$got" = "502" ] && pass "reply-cap is the default and gives 502 over 4 MiB" \
    || fail "reply-cap is the default and gives 502 over 4 MiB: got '$got'"

start_proxy --origin "$HOST:$EPOLL_PORT" --relay unbounded
got="$(fetch_big "$WORK/big_unbounded")"
[ "$got" = "200 $big_sum" ] \
    && pass "unbounded serves a 6 MiB file whole" \
    || fail "unbounded serves a 6 MiB file whole: got '$got'"

# Two slow downloads hold the two slots. The third client gets a 503.
start_proxy --origin "$HOST:$EPOLL_PORT" --relay conn-cap --relay-conns 2
slow_pids=()
for i in 1 2; do
    fetch_big "$WORK/big_conn$i" --limit-rate 2M > "$WORK/conn$i" &
    slow_pids+=($!)
done
sleep 1
got="$(fetch "$BASE/big.bin" | cut -d' ' -f1)"
wait "${slow_pids[@]}"
[ "$got" = "503" ] && pass "conn-cap gives 503 to client N+1" \
    || fail "conn-cap gives 503 to client N+1: got '$got'"
[ "$(cat "$WORK/conn1")" = "200 $big_sum" ] \
    && [ "$(cat "$WORK/conn2")" = "200 $big_sum" ] \
    && pass "conn-cap serves the N slow downloads whole" \
    || fail "conn-cap serves the N slow downloads whole: $(cat "$WORK/conn1"), $(cat "$WORK/conn2")"
got="$(fetch_big "$WORK/big_conn3")"
[ "$got" = "200 $big_sum" ] && pass "conn-cap frees a slot when its download ends" \
    || fail "conn-cap frees a slot when its download ends: got '$got'"

# 6 MiB at 1 MB/s takes about 6 s, longer than --origin-timeout 1.
start_proxy --origin "$HOST:$EPOLL_PORT" --relay backpressure
got="$(fetch_big "$WORK/big_slow" --limit-rate 1M)"
[ "$got" = "200 $big_sum" ] \
    && pass "backpressure gives a slow client the file whole, past --origin-timeout" \
    || fail "backpressure gives a slow client the file whole, past --origin-timeout: got '$got'"
grep -q '^127\.0\.0\.1 [^ ]* pass - 0 200 62915' "$WORK/rproxy.err" \
    && pass "backpressure logs the status from the first Origin bytes" \
    || fail "backpressure logs the status from the first Origin bytes"

# 20 slow clients: with whole replies rproxy would hold 20 x 6 MiB.
slow_pids=()
for i in $(seq 1 20); do
    fetch_big "$WORK/big_many$i" --limit-rate 1M > "$WORK/many$i" &
    slow_pids+=($!)
done
peak_rss=0
while kill -0 "${slow_pids[0]}" 2>/dev/null; do
    rss=$(awk '/^VmRSS/{print $2}' "/proc/$PROXY_PID/status" 2>/dev/null)
    [ "${rss:-0}" -gt "$peak_rss" ] && peak_rss=$rss
    sleep 0.2
done
wait "${slow_pids[@]}"
whole=0
for i in $(seq 1 20); do
    [ "$(cat "$WORK/many$i")" = "200 $big_sum" ] && whole=$((whole + 1))
done
[ "$whole" = "20" ] && [ "$peak_rss" -lt 40960 ] \
    && pass "backpressure serves 20 slow clients under 40 MiB RSS (peak $((peak_rss / 1024)) MiB)" \
    || fail "backpressure serves 20 slow clients under 40 MiB RSS: $whole whole, peak ${peak_rss} KiB"

# --relay-stats: a slow download is open for about 6 s. A snapshot taken
# during it must list the curl port with the reply bytes on the heap.
mkdir -p "$WORK/stats"
start_proxy --origin "$HOST:$EPOLL_PORT" --relay unbounded --loops 2 \
    --relay-stats "$WORK/stats"
curl -sk --max-time 30 --limit-rate 1M -o /dev/null -w '%{local_port}' \
    "$BASE/big.bin" > "$WORK/stats_port" &
stats_pid=$!
sleep 1.5
cat "$WORK"/stats/loop-*.txt > "$WORK/stats_snapshot" 2>/dev/null
wait "$stats_pid"
stats_port=$(cat "$WORK/stats_port")
awk -v port="$stats_port" '$1 == port && $2 > 0 {found = 1} END {exit !found}' \
    "$WORK/stats_snapshot" \
    && pass "--relay-stats lists an open connection by client port with its heap bytes" \
    || fail "--relay-stats lists an open connection by client port with its heap bytes: port '$stats_port', $(tr '\n' '|' < "$WORK/stats_snapshot")"

stop_proxy
"$RPROXY" --port "$PROXY_PORT" --cert build/cert.pem --key build/key.pem \
    --origin "$HOST:$EPOLL_PORT" --relay backpressure --cache on \
    2> "$WORK/bad.err" &
bad_pid=$!
sleep 0.5
if kill -0 "$bad_pid" 2>/dev/null; then
    kill "$bad_pid"; wait "$bad_pid" 2>/dev/null
    fail "backpressure with --cache on stops rproxy at the start"
else
    grep -q 'cache on' "$WORK/bad.err" \
        && pass "backpressure with --cache on stops rproxy at the start" \
        || fail "backpressure with --cache on stops rproxy at the start: $(cat "$WORK/bad.err")"
fi

stop_proxy
for bad in 0 x; do
    "$RPROXY" --port "$PROXY_PORT" --cert build/cert.pem --key build/key.pem \
        --origin "$HOST:$ORIGIN_A" --loops "$bad" 2> /dev/null &
    bad_pid=$!
    sleep 0.5
    if kill -0 "$bad_pid" 2>/dev/null; then
        kill "$bad_pid"; wait "$bad_pid" 2>/dev/null
        fail "--loops $bad stops rproxy at the start"
    else
        pass "--loops $bad stops rproxy at the start"
    fi
done

if [ "$FAILS" -gt 0 ]; then
    echo "--- rproxy stderr ---"
    cat "$WORK/rproxy.err"
    echo "$FAILS failed"
    exit 1
fi
echo "rproxy_func: ok"
