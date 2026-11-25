#!/usr/bin/env bash
# End-to-end tests with real clients (curl, nc, ab). Run on Linux:
#   scripts/dev.sh bash scripts/functional_test.sh
# Topology:
#   :8080  front server  (static + CGI + reverse proxy + forward proxy)
#   :8001  backend "b1"  ┐ round-robin targets for /echo
#   :8002  backend "b2"  ┘
#   :8003  black hole (accepts, never answers) for the timeout case
#   :8081  second front, /echo@ketama over b1/b2 (sticky keys, health checks, cap of 1)
#   :8082  one worker and one queue slot for connection shedding
set -u
cd "$(dirname "$0")/.."
make -s all || exit 1

PASS=0; FAIL=0
ok()   { PASS=$((PASS+1)); echo "  ok   $1"; }
bad()  { FAIL=$((FAIL+1)); echo "  FAIL $1"; }
expect() { # name expected actual
    if [ "$2" = "$3" ]; then ok "$1"; else bad "$1 (expected '$2', got '$3')"; fi
}
code() { curl -s -o /dev/null -w '%{http_code}' "$@"; }
raw()  { printf "$1" | nc -q 2 127.0.0.1 8080 | head -1 | tr -d '\r'; }

LOG=$(mktemp -d)
./webserver -p 8001 -n b1 -r www 2>"$LOG/b1.log" & B1=$!
./webserver -p 8002 -n b2 -r www 2>"$LOG/b2.log" & B2=$!
python3 -c '
import socket, time
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", 8003)); s.listen(16); conns = []
while True: conns.append(s.accept()[0])' & HOLE=$!
./webserver -p 8080 -r www -c cgi-bin -F -T 1000 \
    -R '/echo=127.0.0.1:8001,127.0.0.1:8002' \
    -R '/down=127.0.0.1:8009' \
    -R '/slow=127.0.0.1:8003' 2>"$LOG/front.log" & FRONT=$!
./webserver -p 8081 -B 1 -T 8000 -R '/echo@ketama=127.0.0.1:8001,127.0.0.1:8002' 2>"$LOG/front2.log" & FRONT2=$!
./webserver -p 8082 -t 1 -q 1 -k 10000 2>"$LOG/front3.log" & FRONT3=$!
trap 'kill -CONT $B1 $B2 2>/dev/null; kill $B1 $B2 $HOLE $FRONT $FRONT2 $FRONT3 2>/dev/null; wait' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
sleep 0.5

echo "== static files"
expect "GET / -> 200"            200 "$(code localhost:8080/)"
expect "index is text/html"      "text/html; charset=utf-8" \
    "$(curl -s -o /dev/null -w '%{content_type}' localhost:8080/)"
expect "css mime"                "text/css; charset=utf-8" \
    "$(curl -s -o /dev/null -w '%{content_type}' localhost:8080/css/style.css)"
expect "png mime"                "image/png" \
    "$(curl -s -o /dev/null -w '%{content_type}' localhost:8080/img/logo.png)"
expect "Content-Length matches file" "$(wc -c < www/index.html | tr -d ' ')" \
    "$(curl -s localhost:8080/index.html | wc -c | tr -d ' ')"
expect "HEAD has no body"        0 "$(curl -s -I localhost:8080/ -o /dev/null -w '%{size_download}')"
expect "missing -> 404"          404 "$(code localhost:8080/nope.html)"
expect "traversal -> 400/403/404"    1 "$(c=$(code --path-as-is localhost:8080/../../etc/passwd); [ "$c" = 400 ] || [ "$c" = 403 ] || [ "$c" = 404 ]; echo $((!$?)))"
expect "encoded traversal blocked" 1 "$(c=$(code --path-as-is 'localhost:8080/%2e%2e/%2e%2e/etc/passwd'); [ "$c" = 400 ] || [ "$c" = 403 ] || [ "$c" = 404 ]; echo $((!$?)))"
expect "POST static -> 405"      405 "$(code -d x=1 localhost:8080/index.html)"

echo "== dynamic (CGI)"
expect "CGI GET 200"             200 "$(code 'localhost:8080/cgi-bin/hello.sh?name=jungle')"
curl -s 'localhost:8080/cgi-bin/hello.sh?name=jungle' | grep -q 'name=jungle' \
    && ok "CGI sees QUERY_STRING" || bad "CGI sees QUERY_STRING"
curl -s -d 'msg=hello+world' localhost:8080/cgi-bin/form.py | grep -q 'hello world' \
    && ok "CGI POST body via stdin" || bad "CGI POST body via stdin"
expect "missing CGI -> 404"      404 "$(code localhost:8080/cgi-bin/nope.sh)"

echo "== reverse proxy"
NAMES=$(for i in 1 2 3 4; do curl -s -D - -o /dev/null localhost:8080/echo | \
    tr -d '\r' | awk -F': ' 'tolower($1)=="x-server-name"{print $2}'; done | tr '\n' ' ')
expect "round-robin b1/b2"       "b1 b2 b1 b2 " "$NAMES"
curl -s -d 'payload=42' localhost:8080/echo | grep -q 'payload=42' \
    && ok "POST body relayed" || bad "POST body relayed"
expect "backend down -> 502"     502 "$(code localhost:8080/down/x)"
expect "backend timeout -> 504"  504 "$(code -m 5 localhost:8080/slow/x)"

echo "== forward proxy"
expect "curl -x via proxy"       200 "$(code -x http://127.0.0.1:8080 http://127.0.0.1:8001/index.html)"
curl -s -D - -o /dev/null -x http://127.0.0.1:8080 http://127.0.0.1:8002/ | grep -qi 'x-server-name: b2' \
    && ok "forward proxy reached b2" || bad "forward proxy reached b2"
expect "unreachable origin -> 502" 502 "$(code -x http://127.0.0.1:8080 http://127.0.0.1:8009/)"

echo "== ketama / lb status"
server_from() { # source-ip port -> X-Server-Name
    curl -s -m 2 -D - -o /dev/null --interface "$1" "http://127.0.0.1:$2/echo" | \
        tr -d '\r' | awk -F': ' 'tolower($1)=="x-server-name"{print $2}'
}
for ip in 127.0.0.5 127.0.0.6 127.0.0.7; do
    S=$(for _ in 1 2 3 4 5; do server_from $ip 8081; done | sort -u | tr '\n' ' ')
    case $S in "b1 "|"b2 ") ok "sticky $ip -> $S" ;; *) bad "sticky $ip (got '$S')" ;; esac
done
expect "/_lb/status from loopback -> 200" 200 "$(code localhost:8081/_lb/status)"
curl -s localhost:8081/_lb/status | grep -q ketama \
    && ok "/_lb/status mentions ketama" || bad "/_lb/status mentions ketama"
SELF=$(hostname -I 2>/dev/null | awk '{print $1}')
if [ -n "$SELF" ]; then
    expect "/_lb/status from non-loopback -> 404" 404 "$(code "http://$SELF:8081/_lb/status")"
else
    echo "  skip /_lb/status non-loopback (no non-loopback address)"
fi

echo "== protocol edge cases"
expect "garbage -> 400"          "HTTP/1.1 400 Bad Request" "$(raw 'garbage\r\n\r\n')"
expect "bad version -> 505"      "HTTP/1.1 505 HTTP Version Not Supported" "$(raw 'GET / HTTP/2.0\r\nHost: x\r\n\r\n')"
expect "unknown method -> 501"   "HTTP/1.1 501 Not Implemented" "$(raw 'BREW / HTTP/1.1\r\nHost: x\r\n\r\n')"
expect "1.1 without Host -> 400" "HTTP/1.1 400 Bad Request" "$(raw 'GET / HTTP/1.1\r\n\r\n')"
LONG=$(head -c 5000 /dev/zero | tr '\0' a)
expect "long URI -> 414"         414 "$(code "localhost:8080/$LONG")"
expect "huge body -> 413"        413 "$(head -c 2000000 /dev/zero | code -H 'Expect:' --data-binary @- localhost:8080/echo)"
# keep-alive: two requests on one connection -> two responses
N=$(printf 'GET / HTTP/1.1\r\nHost: x\r\n\r\nGET /css/style.css HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n' \
    | nc -q 2 127.0.0.1 8080 | grep -c '^HTTP/1.1 200')
expect "keep-alive pipelined x2" 2 "$N"
# client vanishes mid-request; server must keep serving
printf 'POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 100\r\n\r\nabc' | nc -q 0 127.0.0.1 8080 >/dev/null
expect "survives abrupt close"   200 "$(code localhost:8080/)"

echo "== concurrency"
ab -q -n 2000 -c 50 -k http://127.0.0.1:8080/index.html > "$LOG/ab.txt" 2>&1
FAILED=$(awk '/^Failed requests/{print $3}' "$LOG/ab.txt")
expect "ab 2000 req / c=50, 0 failed" 0 "${FAILED:-missing}"
grep -E 'Requests per second' "$LOG/ab.txt" | sed 's/^/       /'

echo "== back-pressure"
BACKPRESSURE=http://127.0.0.1:8081
KEYS=()
for i in $(seq 2 64); do
    IP=127.0.0.$i
    if [ "$(server_from "$IP" 8081)" = b2 ]; then KEYS+=("$IP"); fi
    [ "${#KEYS[@]}" -eq 3 ] && break
done

wait_full() {
    local i
    for i in $(seq 20); do
        curl -s -m 0.2 "$BACKPRESSURE/_lb/status" | awk -v addr="127.0.0.1:$1" '
            $1 == addr && $2 == "UP" {
                for (j = 3; j < NF; j++) if ($j == "1/1" && $(j + 1) == "FULL") found = 1
            }
            END { exit !found }' && return 0
        sleep 0.05
    done
    return 1
}

if [ "${#KEYS[@]}" -eq 3 ] && kill -STOP "$B2"; then
    code -m 12 --interface "${KEYS[0]}" "$BACKPRESSURE/echo" >"$LOG/held-b2.code" & HOLD_B2=$!
    wait_full 8002 && ok "paused b2 stays UP and FULL" || bad "paused b2 stays UP and FULL"
    expect "b2 key spills -> 200" 200 \
        "$(code -m 2 --interface "${KEYS[1]}" -D "$LOG/spill.headers" "$BACKPRESSURE/echo")"
    grep -qi '^X-Server-Name: b1' "$LOG/spill.headers" \
        && ok "spill reached b1" || bad "spill reached b1"

    # Hold b1's slot too so the 503 does not depend on racing a fast /echo.
    if kill -STOP "$B1"; then
        code -m 12 --interface "${KEYS[1]}" "$BACKPRESSURE/echo" >"$LOG/held-b1.code" & HOLD_B1=$!
        wait_full 8001 && ok "paused b1 stays UP and FULL" || bad "paused b1 stays UP and FULL"
        expect "GET / while both backends full -> 200" 200 "$(code -m 1 "$BACKPRESSURE/")"
        expect "both backends full -> 503" 503 \
            "$(code -m 1 --interface "${KEYS[2]}" -D "$LOG/shed.headers" "$BACKPRESSURE/echo")"
        grep -qi '^Retry-After: [0-9]' "$LOG/shed.headers" \
            && ok "503 carries Retry-After" || bad "503 carries Retry-After"
        kill -CONT "$B1" "$B2"
        wait "$HOLD_B1"
        expect "held b1 request completes after resume" 200 "$(cat "$LOG/held-b1.code")"
    else
        bad "pause b1"
        kill -CONT "$B2"
    fi
    wait "$HOLD_B2"
    expect "held b2 request completes after resume" 200 "$(cat "$LOG/held-b2.code")"
else
    bad "find three b2 keys and pause b2"
fi

echo "== connection queue shedding"
queue_shed_counts() {
    curl -fsS -m 2 http://127.0.0.1:8082/_lb/status | awk '
        NR == 1 { for (i = 1; i + 4 <= NF; i++)
            if ($i == "shed" && $(i + 2) == "(queue)" && $(i + 4) == "(stale)")
                print $(i + 1), $(i + 3) }'
}
expect "queue/stale shed counters start at zero" "0 0" "$(queue_shed_counts)"
python3 -c '
from http.client import HTTPResponse
from socket import create_connection, SHUT_WR
from time import sleep

def connect():
    return create_connection(("127.0.0.1", 8082), timeout=4)

def check_rejected(client, reason):
    with HTTPResponse(client) as response:
        response.begin()
        retry = response.getheader("Retry-After", "")
        assert response.status == 503 and retry.isdigit(), (reason, response.status, retry)
        print(f"       {reason}: {response.status}, Retry-After: {retry}")

with connect() as held:
    held.sendall(b"HEAD / HTTP/1.1\r\nHost: localhost\r\n\r\n")
    with HTTPResponse(held, method="HEAD") as response:
        response.begin()
        assert response.status == 200 and response.getheader("Connection") == "keep-alive"

    # Consecutive connects fill the slot and then prove it is full before timing the wait.
    with connect() as queued, connect() as overflow:
        check_rejected(overflow, "queue full")
        sleep(2.1)
        held.shutdown(SHUT_WR)
        check_rejected(queued, "queue stale")
' && ok "both queue rejections carry Retry-After" || bad "both queue rejections carry Retry-After"
expect "queue/stale shed counters each increased by one" "1 1" "$(queue_shed_counts)"

echo "== failover (b1 killed)"
kill $B1; wait $B1 2>/dev/null
CODES=$(for i in 11 12 13 14 15 16; do code -m 3 --interface 127.0.0.$i localhost:8081/echo; echo; done | sort -u | tr '\n' ' ')
expect "ketama skips dead b1"    "200 " "$CODES"
expect "round-robin skips dead b1" "200 200 200 200 " \
    "$(for _ in 1 2 3 4; do code -m 3 localhost:8080/echo; echo; done | tr '\n' ' ')"
for _ in $(seq 25); do
    curl -s localhost:8081/_lb/status | grep '127.0.0.1:8001' | grep -q DOWN && break
    sleep 0.2
done
curl -s localhost:8081/_lb/status | grep '127.0.0.1:8001' | grep -q DOWN \
    && ok "status shows 127.0.0.1:8001 DOWN" || bad "status shows 127.0.0.1:8001 DOWN"

echo "== graceful shutdown"
kill -TERM $FRONT $FRONT2
for _ in 1 2 3 4 5 6 7 8 9 10; do kill -0 $FRONT 2>/dev/null || kill -0 $FRONT2 2>/dev/null || break; sleep 0.3; done
kill -0 $FRONT 2>/dev/null && bad "SIGTERM exits" || ok "SIGTERM exits"
kill -0 $FRONT2 2>/dev/null && bad "SIGTERM exits (front2)" || ok "SIGTERM exits (front2)"

echo
echo "passed $PASS, failed $FAIL"
[ $FAIL -eq 0 ]
