#!/usr/bin/env bash
# Load-balancer demo: one Ketama front (:8080) and backends bN on :8000+N (default b1..b3).
#   scripts/lb.sh up | down | add | remove bN | start NAME | stop NAME | pause bN | resume bN
#                 | slow bN MS | status | keys [N]
# NAME is front or a member backend. The member list is .lb/backends, the front reads its /echo backends
# from .lb/front.route (add/remove rewrite it and send SIGHUP); pid files and logs live in .lb/.
# Front options come from env: LB_CAP (-B) LB_WAIT (-W) LB_TIMEOUT (-T) LB_THREADS (-t) LB_STRATEGY LB_BACKENDS.
set -u
cd "$(dirname "$0")/.."
DIR=.lb
MEMBERS=$DIR/backends
MAXB=79   # slot bN is :8000+N; b79 (:8079) is the last port below the front's :8080
# The front takes at most ROUTE_MAX_BACKENDS backends per route; read it so the two never drift.
MAXN=$(awk '$1 == "#define" && $2 == "ROUTE_MAX_BACKENDS" {print $3}' include/route.h)
MAXN=${MAXN:-8}
FRONT=http://127.0.0.1:8080
LB_CAP=${LB_CAP:-2} LB_WAIT=${LB_WAIT:-2000} LB_TIMEOUT=${LB_TIMEOUT:-1000}
LB_THREADS=${LB_THREADS:-8} LB_STRATEGY=${LB_STRATEGY:-ketama}

# BACKENDS is "b1 b2 b3 " (sorted, trailing space): from .lb/backends, else b1..b$LB_BACKENDS.
sorted() { tr ' ' '\n' | grep . | sort -V | tr '\n' ' '; }
defaults() {
    local n=${LB_BACKENDS:-3}
    case $n in ''|*[!0-9]*) n=0 ;; esac
    [ "$((10#$n))" -ge 1 ] && [ "$((10#$n))" -le "$MAXN" ] ||
        { echo "LB_BACKENDS must be 1..$MAXN (got '${LB_BACKENDS:-}')" >&2; exit 1; }
    seq "$((10#$n))" | sed 's/^/b/' | sorted
}
# defaults exits only its $(...) subshell, so every caller checks it.
if [ -s "$MEMBERS" ]; then BACKENDS=$(sorted <"$MEMBERS"); else BACKENDS=$(defaults) || exit 1; fi
NAMES="${BACKENDS}front"
ROUTE=$DIR/front.route   # the front's /echo backend list, one host:port per line; re-read on SIGHUP
save_members() {
    NAMES="${BACKENDS}front"
    write_route && echo "$BACKENDS" >"$MEMBERS" ||
        { echo "could not write $ROUTE or $MEMBERS"; return 1; }
}
addrs() { for b in $BACKENDS; do echo "127.0.0.1:$(port "$b")"; done; }
# tmp + mv: the front never reads a half-written file.
write_route() { addrs >"$ROUTE.tmp" && mv -f "$ROUTE.tmp" "$ROUTE"; }
now_ms() { date +%s%3N; }

usage() {
    cat <<'EOF'
usage: scripts/lb.sh COMMAND
  up              start front :8080 (/echo) and backends b1..bN (:8001..:800N)
  down            stop everything and forget added backends
  add             start a backend on the lowest free bN (:8000+N) and reload
                  the front's backend list (SIGHUP) so it routes there too
  remove bN       drop bN from the front's route (SIGHUP reload), then stop bN
  start NAME      start one of: front, a member backend (a slow bN is replaced by the webserver)
  stop NAME       stop one of: front, a member backend (it stays in the route, shown DOWN)
  pause bN        freeze one backend
  resume bN       continue a paused backend
  slow bN MS      replace bN with tools/slow_backend.py, which answers after MS ms
  status          print the front's options and its /_lb/status table
  keys [N]        send /echo from N source IPs (default 20), show which backend
                  each key hits and how many moved since the last run
front options from env (read by up and start front):
  LB_CAP=2 LB_WAIT=2000 LB_TIMEOUT=1000 LB_THREADS=8 LB_STRATEGY=ketama|rr
  LB_CAP=0 LB_WAIT=0 turns back-pressure off
  LB_BACKENDS=3   backends that up starts (1..ROUTE_MAX_BACKENDS, 8) when the front isn't running yet;
                  after that use add / remove
EOF
    exit 1
}

port() { case $1 in front) echo 8080 ;; *) echo "$((8000 + ${1#b}))" ;; esac; }

route_sep() { case $LB_STRATEGY in ketama) echo @ketama= ;; *) echo = ;; esac; }

args() {
    case $1 in
        front) echo "-p 8080 -r www -c cgi-bin -B $LB_CAP -W $LB_WAIT -T $LB_TIMEOUT -t $LB_THREADS" \
                    "-R /echo$(route_sep)@$ROUTE" ;;
        *) echo "-p $(port "$1") -n $1 -r www" ;;
    esac
}

off() { if [ "$1" = 0 ]; then echo off; else echo "$1${2:-}"; fi; }

save_opts() {
    echo "cap $(off "$LB_CAP")  wait $(off "$LB_WAIT" ms)  timeout ${LB_TIMEOUT}ms  threads $LB_THREADS  $LB_STRATEGY  backends $(echo $BACKENDS | wc -w)" \
        >"$DIR/front.opts"
}

proc_state() {
    local st
    case $1 in ''|*[!0-9]*) return 1 ;; esac
    st=$(cat "/proc/$1/stat" 2>/dev/null) || return 1
    st=${st##*) }                    # state is the first field after "(comm) "
    [ -n "$st" ] && printf '%s\n' "${st%% *}"
}

# kill -0 alone counts zombies (parent gone, nobody reaped yet) as alive.
alive() {
    local st
    st=$(proc_state "$1") && [ "$st" != Z ]
}

pid_of() { # prints the pid if NAME is running, else clears a stale pid file
    local f="$DIR/$1.pid" pid
    [ -f "$f" ] || return 1
    pid=$(cat "$f")
    if alive "$pid"; then echo "$pid"; else rm -f "$f" "$DIR/$1.slow"; return 1; fi
}

# A bare connect, not an HTTP request: a slow backend would not answer within the poll.
accepts() { (exec 3<>"/dev/tcp/127.0.0.1/$(port "$1")") 2>/dev/null; }

start() { # NAME [MS]: with MS, run tools/slow_backend.py instead of the webserver
    local pid what=webserver
    if pid=$(pid_of "$1"); then
        if [ -n "${2:-}" ] || [ ! -f "$DIR/$1.slow" ]; then echo "$1 is already running (pid $pid)"; return 0; fi
        echo "$1 is running slow_backend.py, replacing it with the webserver"
        stop "$1" >/dev/null
    fi
    if accepts "$1"; then echo "port $(port "$1") is already in use"; return 1; fi
    if [ -n "${2:-}" ]; then
        what="slow_backend ${2}ms"
        nohup python3 tools/slow_backend.py "$(port "$1")" "$1" "$2" >"$DIR/$1.log" 2>&1 &
        pid=$!
        echo "$2" >"$DIR/$1.slow"
    else
        [ "$1" = front ] && { save_opts; write_route; }
        # shellcheck disable=SC2046  # args are intentionally word-split
        nohup ./webserver $(args "$1") >"$DIR/$1.log" 2>&1 &
        pid=$!
    fi
    echo "$pid" >"$DIR/$1.pid"
    for _ in $(seq 50); do
        if ! alive "$pid"; then echo "$1 failed to start, see $DIR/$1.log"; rm -f "$DIR/$1.pid" "$DIR/$1.slow"; return 1; fi
        # Re-check the pid: the connect could have reached someone else who grabbed the port.
        accepts "$1" && alive "$pid" &&
            { echo "$1 started (pid $pid, :$(port "$1"), $what)"; return 0; }
        sleep 0.1
    done
    echo "$1 did not accept connections in 5 s, see $DIR/$1.log"; return 1
}

stop() {
    local pid
    if ! pid=$(pid_of "$1"); then echo "$1 is not running"; return 0; fi
    kill -CONT "$pid" 2>/dev/null
    kill -TERM "$pid"
    for _ in $(seq 50); do alive "$pid" || break; sleep 0.1; done
    alive "$pid" && kill -KILL "$pid"
    rm -f "$DIR/$1.pid" "$DIR/$1.slow"
    echo "$1 stopped"
}

freeze() {
    local pid
    if ! pid=$(pid_of "$2"); then echo "$2 is not running"; return 0; fi
    if [ "$1" = pause ]; then
        if [ "$(proc_state "$pid")" = T ]; then echo "$2 is already paused"; return 0; fi
        kill -STOP "$pid" && echo "$2 paused"
    else
        kill -CONT "$pid" && echo "$2 resumed"
    fi
}

keys() {
    local n=${1:-20} i key name prev moved=0 cmp=0 cur
    case $n in ''|*[!0-9]*) usage ;; esac
    [ "$n" -ge 1 ] && [ "$n" -le 254 ] || { echo "N must be 1..254"; exit 1; }
    cur=$(mktemp)
    printf '%-12s %s\n' KEY BACKEND
    for i in $(seq "$n"); do
        key=127.0.0.$((i + 1))
        name=$(curl -s -m 2 -D - -o /dev/null --interface "$key" "$FRONT/echo" |
            tr -d '\r' | awk -F': ' 'tolower($1)=="x-server-name"{print $2}')
        name=${name:-ERR}
        prev=$( [ -f "$DIR/keys.prev" ] && awk -v k="$key" '$1==k && $2!="ERR"{print $2}' "$DIR/keys.prev")
        if [ "$name" = ERR ]; then
            [ -z "$prev" ] || echo "$key $prev" >>"$cur"
            printf '%-12s %s\n' "$key" "$name"
            continue
        fi
        echo "$key $name" >>"$cur"
        if [ -z "$prev" ]; then
            printf '%-12s %s\n' "$key" "$name"
        else
            cmp=$((cmp + 1))
            if [ "$prev" = "$name" ]; then printf '%-12s %s\n' "$key" "$name"
            else moved=$((moved + 1)); printf '%-12s %s → %s  (moved)\n' "$key" "$prev" "$name"; fi
        fi
    done
    if [ "$cmp" -eq 0 ]; then echo "no keys to compare"
    else echo "moved $moved/$cmp ($((moved * 100 / cmp))%)"; fi
    mv "$cur" "$DIR/keys.prev"
}

valid() { case " $NAMES " in *" ${1:-} "*) ;; *) usage ;; esac; }
backend() { case " $BACKENDS " in *" ${1:-} "*) ;; *) echo "${1:-?} is not a backend (members: $BACKENDS)"; exit 1 ;; esac; }

# Old way: a front started with the list inline (or an old binary) only knows its start-up route.
refront() {
    pid_of front >/dev/null || return 0
    stop front >/dev/null
    start front
}

# The route file is already rewritten; tell the front to re-read it and wait until /_lb/status shows it.
reload() {
    local pid want
    pid=$(pid_of front) || return 0
    # An old front dies on SIGHUP; only a front started on the file (-R ...=@file) gets one.
    tr '\0' ' ' <"/proc/$pid/cmdline" 2>/dev/null | grep -q '=@' || { refront; return; }
    kill -HUP "$pid" || { echo "could not send SIGHUP to the front (pid $pid)"; return 1; }
    want=$(addrs)
    local end=$(( $(now_ms) + 2000 )) left
    while left=$(( end - $(now_ms) )); [ "$left" -gt 0 ]; do
        # Every status request gets only the time left, so the whole wait stays within 2 s.
        [ "$(curl -s -m "$(printf '%d.%03d' $((left / 1000)) $((left % 1000)))" "$FRONT/_lb/status" |
             awk '$1 ~ /^127\.0\.0\.1:/ {print $1}')" = "$want" ] &&
            { echo "front reloaded (pid $pid, :8080, webserver)"
              # Only the count changed: keep the options the running front was started with.
              [ -f "$DIR/front.opts" ] && sed -i "s/backends [0-9]*$/backends $(echo $BACKENDS | wc -w)/" "$DIR/front.opts"
              return 0; }
        [ "$left" -gt 100 ] && sleep 0.1
    done
    echo "front did not show the new backend list in 2 s, see $DIR/front.log"; return 1
}

# BACKENDS is already the new list. Write it and reload; on any failure put OLD ($1) back
# in both files and in the front, so nothing changed and the command can be retried.
apply() {
    save_members && reload && return 0
    BACKENDS=$1
    save_members >/dev/null && reload >/dev/null
    echo "member change not applied; backends stay: $BACKENDS"
    return 1
}

# Requests the front sent before the reload still run on the old table; a slow_backend.py exits
# at once on TERM, so give them the front's proxy timeout (-T) first. The webserver finishes them itself.
drain() {
    local pid ms
    [ -f "$DIR/$1.slow" ] && pid=$(pid_of front) || return 0
    ms=$(tr '\0' '\n' <"/proc/$pid/cmdline" 2>/dev/null | awk 'p {print; exit} $0 == "-T" {p = 1}')
    case $ms in ''|*[!0-9]*) ms=$LB_TIMEOUT ;; esac
    echo "waiting ${ms}ms for requests already sent to $1"
    sleep "$(printf '%d.%03d' $((ms / 1000)) $((ms % 1000)))"
}

add() {
    local n name
    [ "$(echo $BACKENDS | wc -w)" -lt "$MAXN" ] ||
        { echo "the front takes at most $MAXN backends (ROUTE_MAX_BACKENDS); remove one first"; return 1; }
    for n in $(seq $MAXB); do
        name=b$n
        case " $BACKENDS " in *" $name "*) continue ;; esac
        accepts "$name" && continue          # someone else already listens on :800N
        start "$name" || return 1
        local old=$BACKENDS
        BACKENDS=$(echo "$BACKENDS $name" | sorted)
        apply "$old" || { stop "$name"; return 1; }
        echo "backends: $BACKENDS"
        return 0
    done
    echo "no free backend slot (b1..b$MAXB, :8001..:80$MAXB)"; return 1
}

remove() {
    backend "$1"
    [ "$(echo $BACKENDS | wc -w)" -gt 1 ] || { echo "$1 is the last backend; use down instead"; return 1; }
    local old=$BACKENDS
    BACKENDS=$(for b in $BACKENDS; do [ "$b" = "$1" ] || echo "$b"; done | sorted)
    # Out of the route first, so the front sends it nothing new; stop only once that took effect.
    apply "$old" || return 1
    drain "$1"
    stop "$1"
    echo "backends: $BACKENDS"
}

[ $# -ge 1 ] || usage
case $LB_STRATEGY in ketama|rr) ;; *) echo "LB_STRATEGY must be ketama or rr (got '$LB_STRATEGY')"; exit 1 ;; esac
mkdir -p "$DIR"
case $1 in
    up)     make -s all || exit 1
            # A fresh setup takes LB_BACKENDS; a running one keeps its members (added ones too).
            pid_of front >/dev/null || { BACKENDS=$(defaults) || exit 1; save_members || exit 1; }
            for n in $NAMES; do start "$n" || exit 1; done ;;
    # down stops every bN with a pid file, members or not, and forgets the member list.
    down)   stop front
            for f in "$DIR"/b*.pid; do [ -e "$f" ] && stop "$(basename "$f" .pid)"; done
            rm -f "$DIR"/*.pid "$DIR"/*.slow "$MEMBERS" "$ROUTE" ;;
    add)    make -s all || exit 1; add ;;
    # Build first: an old front is restarted (refront) on the new binary, which knows -R ...=@file.
    remove) backend "${2:-}"; make -s all || exit 1; remove "$2" ;;
    start)  valid "${2:-}"; make -s all || exit 1; start "$2" ;;
    stop)   valid "${2:-}"; stop "$2" ;;
    pause|resume)
            backend "${2:-}"; freeze "$1" "$2" ;;
    slow)   backend "${2:-}"
            case ${3:-} in ''|*[!0-9]*) usage ;; esac
            stop "$2" >/dev/null; start "$2" "$3" ;;
    status) [ -f "$DIR/front.opts" ] && cat "$DIR/front.opts"
            curl -sf "$FRONT/_lb/status" || { echo "front is not answering /_lb/status"; exit 1; } ;;
    keys)   keys "${2:-20}" ;;
    *)      usage ;;
esac
