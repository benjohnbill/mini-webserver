#!/usr/bin/env bash
# End-to-end tests with real clients (curl, nc, ab). Run on Linux:
#   scripts/dev.sh bash scripts/functional_test.sh
# Topology:
#   :8080  front server  (static + CGI + reverse proxy + forward proxy)
#   :8001  backend "b1"  ┐ round-robin targets for /echo
#   :8002  backend "b2"  ┘
#   :8003  black hole (accepts, never answers) for the timeout case
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
trap 'kill $B1 $B2 $HOLE $FRONT 2>/dev/null' EXIT
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

echo "== graceful shutdown"
kill -TERM $FRONT
for _ in 1 2 3 4 5 6 7 8 9 10; do kill -0 $FRONT 2>/dev/null || break; sleep 0.3; done
kill -0 $FRONT 2>/dev/null && bad "SIGTERM exits" || ok "SIGTERM exits"

echo
echo "passed $PASS, failed $FAIL"
[ $FAIL -eq 0 ]
