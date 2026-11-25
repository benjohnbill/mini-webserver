#!/usr/bin/env bash
# Regression test for scripts/lb.sh, mini-web and bp_demo.sh: weights, N backends, steady clients.
# It owns .lb/ and ports :8001..:8080, so it runs only in a throwaway copy of the repo, never next to a
# live lab (mini-lab uses the repo's .lb/). From Git Bash on the host:
#   MSYS_NO_PATHCONV=1 docker run --rm -v "$(pwd -W):/ro:ro" mini-webserver-tools bash -c 'mkdir /work && cd /ro &&
#     tar --exclude=./.lb --exclude=./build --exclude=./webserver -cf - . | tar -xf - -C /work &&
#     cd /work && LB_TEST_IN_COPY=1 bash scripts/lb_test.sh'
set -u
cd "$(dirname "$0")/.."
[ "${LB_TEST_IN_COPY:-}" = 1 ] ||
    { echo "lb_test.sh: refusing to run outside a throwaway copy (set LB_TEST_IN_COPY=1 there, see the header)"; exit 2; }
make -s all || exit 1
unset LB_BACKENDS LB_WEIGHTS
L="bash scripts/lb.sh" MW="bash scripts/mini-web"
trap '$L down >/dev/null 2>&1' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

PASS=0 FAIL=0
ok()  { PASS=$((PASS + 1)); echo "  ok   $1"; }
bad() { FAIL=$((FAIL + 1)); echo "  FAIL $1"; }
expect() { if [ "$2" = "$3" ]; then ok "$1"; else bad "$1 (expected '$2', got '$3')"; fi; }
# rows: "port:weight ..." from /_lb/status, in table order
rows() { curl -s -m 2 http://127.0.0.1:8080/_lb/status | awk '$1 ~ /^127\.0\.0\.1:/ {sub(/.*:/, "", $1); printf "%s:%s ", $1, $3}'; }
fpid() { cat .lb/front.pid 2>/dev/null; }
# Everything a refused command must leave alone.
snap() { for f in .lb/backends .lb/weights .lb/front.route .lb/front.pid .lb/front.opts; do
    printf '%s=' "$f"; cat "$f" 2>/dev/null | tr '\n' ' '; echo; done; rows; }
refused() { # name command...: rc must be non-zero and snap unchanged
    local name=$1 before after out rc; shift
    before=$(snap); out=$("$@" 2>&1); rc=$?; after=$(snap)
    if [ "$rc" != 0 ] && [ "$before" = "$after" ]; then ok "$name refused: $(tail -1 <<<"$out")"
    else bad "$name refused (rc $rc, state changed: $([ "$before" = "$after" ] && echo no || echo yes), output: $out)"; fi
}
total_ok() { curl -s -m 2 http://127.0.0.1:8080/_lb/status | awk '$1 ~ /^127\.0\.0\.1:/ {s += $8} END {print s + 0}'; }
# "b12" or "127.0.0.1:8012" -> 8012, in the last field
to_port() { awk '{n = $NF; if (n ~ /^b[0-9]+$/) n = 8000 + substr(n, 2); else sub(/.*:/, "", n); $NF = n; print}'; }

echo "== N=1: up / add --weight / weight / remove"
LB_BACKENDS=1 $L up >/dev/null || bad "LB_BACKENDS=1 up"
expect "LB_BACKENDS=1 up -> one row" "8001:1 " "$(rows)"
P=$(fpid)
OUT=$($L add --weight 2)
expect "add --weight 2 says front reloaded" 1 "$(grep -c "^front reloaded (pid $P, :8080, webserver)" <<<"$OUT")"
expect "add --weight 2 -> b2 weight 2"  "8001:1 8002:2 " "$(rows)"
expect "route file has *2"              "127.0.0.1:8001 127.0.0.1:8002*2 " "$(tr '\n' ' ' <.lb/front.route)"
expect ".lb/weights"                    "b2 2" "$(cat .lb/weights)"
$L weight b1 3 >/dev/null || bad "weight b1 3"
expect "weight b1 3 -> status"          "8001:3 8002:2 " "$(rows)"
expect "front.opts weights"             1 "$(grep -c 'backends 2  weights 3,2$' .lb/front.opts)"
$L remove b2 >/dev/null || bad "remove b2"
expect "remove b2 -> status"            "8001:3 " "$(rows)"
expect "remove drops b2's weight line"  "b1 3" "$(cat .lb/weights)"
refused "remove the last backend" $L remove b1
expect "front pid unchanged through N=1" "$P" "$(fpid)"
$L down >/dev/null
expect "down removes the state files" "" "$(ls .lb/backends .lb/weights .lb/front.route 2>/dev/null)"

echo "== N=5: LB_WEIGHTS=1,3,1,2,4 up, keys, mini-web, remove keeps the others"
LB_WEIGHTS=1,3,1,2,4 $L up >/dev/null || bad "LB_WEIGHTS=1,3,1,2,4 up"
expect "count from LB_WEIGHTS"          "8001:1 8002:3 8003:1 8004:2 8005:4 " "$(rows)"
expect "front.opts"                     1 "$(grep -c 'backends 5  weights 1,3,1,2,4$' .lb/front.opts)"
P=$(fpid)
KEYS=$($L keys 60)
SUMMARY=$(grep -E '^b[0-9]+  weight [1-4]  keys [0-9]+ \([0-9]+%\)$' <<<"$KEYS")
expect "keys summary has a line per member" 5 "$(grep -c . <<<"$SUMMARY")"
echo "$SUMMARY" | sed 's/^/       /'
# The weighted share must show: b5 (weight 4) takes more keys than each weight-1 member.
K() { awk -v b="$1" '$1 == b {print $5}' <<<"$SUMMARY"; }
[ "$(K b5)" -gt "$(K b1)" ] && [ "$(K b5)" -gt "$(K b3)" ] && [ "$(K b2)" -gt "$(K b1)" ] \
    && ok "keys: weight 4 / 3 members take more keys than weight 1" || bad "keys proportional to weight ($(tr '\n' ';' <<<"$SUMMARY"))"
if [ -f tools/lbsim.py ]; then
    WANT=$(python3 tools/lbsim.py --backends 5 --weights 1,3,1,2,4 --map 2>&1 |
        awk '{split($1, o, "."); if (o[1] o[2] o[3] == "12700" && o[4] >= 2 && o[4] <= 61) print $2}' | to_port | sort | uniq -c | awk '{print $2, $1}')
    GOT=$(awk '{print $1, $5}' <<<"$SUMMARY" | awk '{print 8000 + substr($1, 2), $2}' | awk '$2 > 0' | sort)
    expect "keys summary = lbsim --map counts (60 keys)" "$WANT" "$GOT"
else
    bad "tools/lbsim.py missing"
fi
OUT=$($MW backend new --weight 4)
expect "mini-web backend new --weight 4" "8001:1 8002:3 8003:1 8004:2 8005:4 8006:4 " "$(rows)"
grep -q '^backends: b1 b2 b3 b4 b5 b6 $' <<<"$OUT" && ok "mini-web prints the lb.sh lines" || bad "mini-web output ($OUT)"
$MW weight b6 1 >/dev/null || bad "mini-web weight b6 1"
expect "mini-web weight b6 1"           "8001:1 8002:3 8003:1 8004:2 8005:4 8006:1 " "$(rows)"
$L remove b3 >/dev/null || bad "remove b3"
expect "remove b3 keeps the other weights" "8001:1 8002:3 8004:2 8005:4 8006:1 " "$(rows)"
expect ".lb/weights after remove"       "b2 3 b4 2 b5 4 " "$(tr '\n' ' ' <.lb/weights)"
LB_WEIGHTS=4,4 $L up >/dev/null || bad "up while running"
expect "up while running keeps members and ignores LB_WEIGHTS" "8001:1 8002:3 8004:2 8005:4 8006:1 " "$(rows)"
expect "front pid unchanged through N=5" "$P" "$(fpid)"

echo "== bad weights change nothing"
for w in 0 5 x 12 ''; do
    refused "add --weight '$w'" $L add --weight "$w"
    refused "weight b2 '$w'"    $L weight b2 "$w"
done
refused "mini-web backend new --weight 9" $MW backend new --weight 9
refused "weight of a non-member"          $L weight b7 2
expect "no backend b7 started" "" "$(ls .lb/b7.pid 2>/dev/null)"

echo "== a change that fails after validation is rolled back (files and the live front)"
P=$(fpid) ROWS=$(rows) BEFORE=$(snap)
# 1. weights.tmp is a directory: the route file is written, the weights file is not -> rollback.
mkdir .lb/weights.tmp
out=$($L weight b2 1 2>&1); rc=$?
rmdir .lb/weights.tmp
[ "$rc" = 1 ] && grep -q '^member change not applied; backends stay: b1 b2 b4 b5 b6 ' <<<"$out" \
    && ok "write failure: weight b2 1 reports not applied" || bad "write failure: weight b2 1 (rc $rc: $out)"
expect "write failure: files and front list back as before" "$BEFORE" "$(snap)"
# 2. a new backend whose route write fails is stopped again.
mkdir .lb/front.route.tmp
out=$($L add --weight 2 2>&1); rc=$?
rmdir .lb/front.route.tmp
[ "$rc" = 1 ] && grep -q '^b3 stopped$' <<<"$out" && ok "write failure: add stops the new b3 (lowest free slot)" || bad "write failure: add (rc $rc: $out)"
expect "write failure: add leaves everything as before" "$BEFORE" "$(snap)"
expect "write failure: no b3 left" "" "$(ls .lb/b3.pid 2>/dev/null)"
# 3. a frozen front confirms neither the change nor the rollback: say so, put the files back.
kill -STOP "$P"
out=$($L weight b2 2 2>&1); rc=$?
kill -CONT "$P"
[ "$rc" = 1 ] && grep -q '^rollback failed: the front does not show the old list' <<<"$out" \
    && ok "frozen front: reports rollback failed" || bad "frozen front (rc $rc: $out)"
for _ in $(seq 20); do [ "$(rows)" = "$ROWS" ] && break; sleep 0.1; done
expect "frozen front: once resumed it shows the old list" "$ROWS" "$(rows)"
expect "frozen front: files back as before" "$BEFORE" "$(snap)"
expect "rollback checks: front pid unchanged" "$P" "$(fpid)"

echo "== old inline front: bad front options are refused before it is stopped"
$L stop front >/dev/null
# shellcheck disable=SC2046  # the list is meant to split
nohup ./webserver -p 8080 -r www -c cgi-bin -R "/echo@ketama=$(for b in $(cat .lb/backends); do
    w=$(awk -v b="$b" '$1 == b {print $2}' .lb/weights); printf '127.0.0.1:%s%s,' $((8000 + ${b#b})) "${w:+*$w}"; done | sed 's/,$//')" \
    >.lb/front.log 2>&1 &
echo $! >.lb/front.pid; sleep 0.5
P=$(fpid) BEFORE=$(snap)
out=$(LB_CAP=abc $L weight b2 1 2>&1); rc=$?
[ "$rc" = 1 ] && grep -q "LB_CAP must be 0..1024 (got 'abc')" <<<"$out" && grep -q '^member change not applied' <<<"$out" \
    && ok "LB_CAP=abc weight on an old front: refused, not applied" || bad "LB_CAP=abc on an old front (rc $rc: $out)"
kill -0 "$P" 2>/dev/null && ok "old front still running (pid $P)" || bad "old front still running"
expect "old front: files and list unchanged" "$BEFORE" "$(snap)"
$L weight b2 1 >/dev/null || bad "weight b2 1 on the old front with good options"
expect "good options: old front replaced by a file front" 1 \
    "$(tr '\0' ' ' </proc/"$(fpid)"/cmdline | grep -c '=@')"
expect "good options: new list" "8001:1 8002:1 8004:2 8005:4 8006:1 " "$(rows)"

echo "== a bad up changes nothing"
$L down >/dev/null
for spec in "LB_WEIGHTS=1,0,1" "LB_WEIGHTS=1,5" "LB_WEIGHTS=1,x" "LB_WEIGHTS=1,,2" "LB_WEIGHTS=1,2," \
            $'LB_WEIGHTS=1,2\n3' "LB_BACKENDS=3 LB_WEIGHTS=1,2" "LB_WEIGHTS=1,1,1,1,1,1,1,1,1" "LB_BACKENDS=0" \
            $'LB_BACKENDS=2\n' "LB_CAP=x" "LB_STRATEGY=nope"; do
    before=$(ls -A .lb | sort | tr '\n' ' ')
    # Split on spaces only: a value may hold a newline on purpose.
    IFS=' ' read -r -d '' -a assigns < <(printf '%s' "$spec")
    out=$(env "${assigns[@]}" bash scripts/lb.sh up 2>&1); rc=$?
    [ "$rc" != 0 ] && [ "$before" = "$(ls -A .lb | sort | tr '\n' ' ')" ] \
        && ok "up with $(printf %q "$spec") refused, no file created: $(tail -1 <<<"$out")" \
        || bad "up with $(printf %q "$spec") (rc $rc, files ${before} -> $(ls -A .lb | tr '\n' ' '): $out)"
done

echo "== up with a member list keeps it (front stopped, or a restarted lab); down -> up starts fresh"
$L up >/dev/null && $L add --weight 3 >/dev/null || bad "up, add --weight 3"
$L stop front >/dev/null
out=$(LB_BACKENDS=5 LB_WEIGHTS=1,5,1 $L up 2>&1 >/dev/null); rc=$?
expect "front stopped: up rc (LB_BACKENDS / LB_WEIGHTS not used)" 0 "$rc"
expect "front stopped: up says it keeps the members" "keeping members from .lb/backends (lb.sh down to reset)" "$out"
expect "front stopped: up brings back members and weights" "8001:1 8002:1 8003:1 8004:3 " "$(rows)"
expect "front stopped: route file kept *3" "127.0.0.1:8004*3" "$(tail -1 .lb/front.route)"
# The same with a dead front whose pid file was left behind (the lab container went away).
kill -9 "$(fpid)"; sleep 0.3
$L up >/dev/null 2>&1 || bad "up after the front was killed"
expect "dead front + stale pid: up keeps members and weights" "8001:1 8002:1 8003:1 8004:3 " "$(rows)"
$L down >/dev/null
$L up >/dev/null || bad "down, up"
expect "down -> up: defaults again (b1..b3, weight 1)" "8001:1 8002:1 8003:1 " "$(rows)"
expect "down -> up: no weights file lines" "" "$(cat .lb/weights)"
$L down >/dev/null
# down, then only start front: a fresh up writes the new list and reloads that running front.
$L start front >/dev/null; P=$(fpid)
OUT=$(LB_WEIGHTS=1,3 $L up 2>&1); rc=$?
expect "running front + fresh up: rc" 0 "$rc"
grep -q "^front reloaded (pid $P, :8080, webserver)$" <<<"$OUT" && ok "running front + fresh up: reloaded" || bad "running front + fresh up ($OUT)"
expect "running front + fresh up: front uses the new list" "8001:1 8002:3 " "$(rows)"
expect "running front + fresh up: same front" "$P" "$(fpid)"
$L down >/dev/null

echo "== N=8: the 9th add is refused, weights still work at the limit"
LB_BACKENDS=8 $L up >/dev/null || bad "LB_BACKENDS=8 up"
expect "8 rows" 8 "$(rows | wc -w)"
P=$(fpid)
refused "9th add" $L add
refused "9th add --weight 2" $L add --weight 2
$L weight b8 4 >/dev/null || bad "weight b8 4"
expect "weight b8 4" 8008:4 "$(rows | awk '{print $8}')"
$L remove b8 >/dev/null && $L add --weight 2 >/dev/null || bad "remove b8, add --weight 2"
expect "b8 back with weight 2" 8008:2 "$(rows | awk '{print $8}')"
expect "front pid unchanged through N=8" "$P" "$(fpid)"

echo "== add / remove under load keep the front and lose nothing"
$L down >/dev/null; $L up >/dev/null
P=$(fpid)
rm -f /tmp/lb_test.stop
( n=0 b=0; while [ ! -f /tmp/lb_test.stop ]; do
    c=$(curl -s -o /dev/null -w '%{http_code}' -m 3 --interface 127.0.0.$((n % 50 + 2)) http://127.0.0.1:8080/echo)
    [ "$c" = 200 ] || b=$((b + 1)); n=$((n + 1)); done; echo "$n $b" >/tmp/lb_test.load ) & LOAD=$!
sleep 0.3
$L add --weight 3 >/dev/null && $L weight b1 2 >/dev/null && $L remove b2 >/dev/null || bad "add/weight/remove under load"
sleep 0.3; touch /tmp/lb_test.stop; wait $LOAD
read -r N NON200 </tmp/lb_test.load
expect "under load: $N requests, non-200" 0 "$NON200"
expect "under load: front pid unchanged" "$P" "$(fpid)"
expect "under load: final rows" "8001:2 8003:1 8004:3 " "$(rows)"

echo "== steady clients"
CL=$($L client new 20); expect "client new 20" "clients: 20 (rps 2)" "$CL"
CPID=$(cat .lb/clients.pid 2>/dev/null)
kill -0 "$CPID" 2>/dev/null && ok "client daemon running (pid $CPID)" || bad "client daemon running"
B=$(total_ok); sleep 2.5; A=$(total_ok)
[ "$A" -gt "$B" ] && ok "status OK counters grow with clients ($B -> $A)" || bad "status OK counters grow ($B -> $A)"
LS=$($L client ls)
grep -q '^clients 20  rps 2' <<<"$LS" && ok "client ls: clients 20" || bad "client ls ($LS)"
grep -q '^total  sent [0-9]' <<<"$LS" && ok "client ls: total line" || bad "client ls total line"
echo "$LS" | sed 's/^/       /'
expect "client rm 5"            "clients: 15 (rps 2)" "$($L client rm 5)"
before=$(cat .lb/clients)
out=$($L client new 236); rc=$?
[ "$rc" != 0 ] && [ "$before" = "$(cat .lb/clients)" ] && ok "client new past 250 refused: $out" || bad "client new past 250 (rc $rc: $out)"
for r in 0 0.05 21 x; do
    out=$($L client new 1 --rps "$r"); rc=$?
    [ "$rc" != 0 ] && [ "$before" = "$(cat .lb/clients)" ] && ok "--rps $r refused" || bad "--rps $r refused (rc $rc: $out)"
done
expect "client new 0 --rps 0.5" "clients: 15 (rps 0.5)" "$($L client new 0 --rps 0.5)"
OUT=$($L client new 10 --rps 10)
grep -q '^clients: 25 (rps 10)$' <<<"$OUT" && grep -q '^warning: ' <<<"$OUT" \
    && ok "25 x 10 rps warns: $(grep '^warning' <<<"$OUT")" || bad "client new 10 --rps 10 ($OUT)"
expect "client rm 100 floors at 0" "clients: 0 (rps 10)" "$($L client rm 100)"
sleep 1.5
grep -q '^clients 0 ' <<<"$($MW client ls)" && ok "mini-web client ls: clients 0" || bad "mini-web client ls ($($MW client ls))"
expect "same daemon throughout" "$CPID" "$(cat .lb/clients.pid 2>/dev/null)"
$L client stop >/dev/null
kill -0 "$CPID" 2>/dev/null && bad "client stop ends the daemon" || ok "client stop ends the daemon"
expect "client stop removes the files" "" "$(ls .lb/clients .lb/clients.stats .lb/clients.pid 2>/dev/null)"
expect "client ls with no daemon" "no clients running" "$($L client ls)"
expect "client new 18446744073709551615 refused (no wrap-around)" 1 \
    "$($L client new 18446744073709551615 >/dev/null; echo $?)"
expect "client rm 18446744073709551615 floors at 0" "clients: 0 (rps 2)" "$($L client rm 18446744073709551615)"
$L client stop >/dev/null
# The daemon cannot start (clientd.py gone in this copy): client new fails and the count stays.
$L client new 2 >/dev/null; CPID=$(cat .lb/clients.pid); kill "$CPID"; wait "$CPID" 2>/dev/null
for _ in $(seq 30); do kill -0 "$CPID" 2>/dev/null || break; sleep 0.1; done
BEFORE=$(cat .lb/clients)
mv tools/clientd.py tools/clientd.py.off
out=$($L client new 3); rc=$?
mv tools/clientd.py.off tools/clientd.py
[ "$rc" = 1 ] && grep -q '^client daemon failed to start' <<<"$out" \
    && ok "daemon start failure: client new 3 fails" || bad "daemon start failure (rc $rc: $out)"
expect "daemon start failure: state back to the old count" "$BEFORE" "$(cat .lb/clients)"
expect "daemon start failure: no pid file" "" "$(ls .lb/clients.pid 2>/dev/null)"
$L client stop >/dev/null
# A daemon killed from outside: client new starts over from 0 with a fresh daemon.
$L client new 2 >/dev/null; CPID=$(cat .lb/clients.pid); kill -9 "$CPID"
for _ in $(seq 30); do kill -0 "$CPID" 2>/dev/null || break; sleep 0.1; done
expect "dead daemon: client new 3 counts from 0" "clients: 3 (rps 2)" "$($L client new 3)"
NPID=$(cat .lb/clients.pid 2>/dev/null)
[ -n "$NPID" ] && [ "$NPID" != "$CPID" ] && kill -0 "$NPID" 2>/dev/null \
    && ok "dead daemon: a new daemon runs (pid $NPID)" || bad "dead daemon: new daemon (pid '$NPID')"
$L client stop >/dev/null
expect "client stop removes stats.tmp too" "" "$(ls .lb/clients.stats.tmp 2>/dev/null)"
# Two client new at once: one daemon, both increments kept.
$L client new 5 >/dev/null & $L client new 5 >/dev/null & wait
expect "parallel client new: count 10" "count 10" "$(grep count .lb/clients)"
expect "parallel client new: one daemon" 1 "$(pgrep -fc 'tools/clientd.py')"
$L client stop >/dev/null
$MW client new 5 >/dev/null; CPID=$(cat .lb/clients.pid 2>/dev/null)
[ -n "$CPID" ] && kill -0 "$CPID" 2>/dev/null && ok "client daemon up before down (pid $CPID)" || bad "client daemon up before down"
$L down >/dev/null
kill -0 "$CPID" 2>/dev/null && bad "down stops the clients" || ok "down stops the clients"
# clients.log stays, like the other .lb/*.log files.
expect "down removes the client files" "" "$(ls .lb/clients .lb/clients.stats .lb/clients.pid 2>/dev/null)"
# Without a member list, a bad LB_BACKENDS must not keep client / down from working.
expect "no member list: client new under LB_BACKENDS=0" "clients: 1 (rps 2)" "$(LB_BACKENDS=0 $L client new 1)"
CPID=$(cat .lb/clients.pid 2>/dev/null)
LB_BACKENDS=0 LB_WEIGHTS=9 $L down >/dev/null
kill -0 "$CPID" 2>/dev/null && bad "LB_BACKENDS=0 down stops the clients" || ok "LB_BACKENDS=0 down stops the clients"
expect "LB_BACKENDS=0 down removes the client files" "" "$(ls .lb/clients .lb/clients.stats .lb/clients.pid 2>/dev/null)"

echo "== bp_demo.sh"
OUT=$(bash scripts/bp_demo.sh --duration 3 2>&1); rc=$?
expect "bp_demo defaults rc" 0 "$rc"
grep -q '^backends 3  weights 1,1,1  target b2$' <<<"$OUT" && ok "bp_demo defaults: header" || bad "bp_demo defaults header"
expect "bp_demo defaults: 9 result rows" 9 "$(grep -cE '^(hung|slow|normal) +[0-9]+ ' <<<"$OUT")"
grep -q 'FAILED' <<<"$OUT" && bad "bp_demo defaults: no FAILED" || ok "bp_demo defaults: no FAILED"
echo "$OUT" | tail -11 | sed 's/^/       /'
OUT=$(bash scripts/bp_demo.sh --backends 5 --target b4 --weights 1,2,1,3,1 --scenarios hung,slow,normal --caps 2 --duration 3 2>&1); rc=$?
expect "bp_demo --backends 5 --target b4 rc" 0 "$rc"
grep -q '^backends 5  weights 1,2,1,3,1  target b4$' <<<"$OUT" && ok "bp_demo N=5: header" || bad "bp_demo N=5 header"
grep -q 'backends 5  weights 1,2,1,3,1$' .lb/front.opts \
    && ok "bp_demo N=5: the front ran with 5 weighted backends ($(cat .lb/front.opts))" || bad "bp_demo N=5 front.opts ($(cat .lb/front.opts 2>/dev/null))"
grep -q '^b4 paused' .lb/run-*-hung-2.setup && ok "bp_demo N=5: hung pauses b4" || bad "bp_demo N=5: hung pauses b4"
grep -q '^b4 started (.*slow_backend' .lb/run-*-slow-2.setup && ok "bp_demo N=5: slow replaces b4" || bad "bp_demo N=5: slow replaces b4"
echo "$OUT" | tail -5 | sed 's/^/       /'
# Bad arguments are refused before bp_demo's lb.sh down: a demo that is already up keeps running.
$L up >/dev/null; P=$(fpid)
for a in "--backends 5 --target b6" "--backends 3 --weights 1,2" "--weights 1,5,1" "--backends 9" "--target x" \
         "--weights" "--weights ''" $'--weights 1,1,1\n1,1,1'; do
    case $a in
        "--weights ''") out=$(bash scripts/bp_demo.sh --weights '' 2>&1); rc=$? ;;
        $'--weights 1,1,1\n1,1,1') out=$(bash scripts/bp_demo.sh --weights $'1,1,1\n1,1,1' 2>&1); rc=$? ;;
        # shellcheck disable=SC2086  # a is an argument list
        *) out=$(bash scripts/bp_demo.sh $a 2>&1); rc=$? ;;
    esac
    [ "$rc" != 0 ] && ok "bp_demo $(printf %q "$a") refused: $(tail -1 <<<"$out")" || bad "bp_demo $(printf %q "$a") refused (rc $rc)"
done
expect "refused bp_demo runs left the running demo alone" "$P" "$(kill -0 "$P" 2>/dev/null && fpid)"
$L down >/dev/null

echo
echo "passed $PASS, failed $FAIL"
[ "$FAIL" -eq 0 ]
