#!/usr/bin/env bash
# Load-balancer demo: one Ketama front (:8080) and three backends (:8001..:8003).
#   scripts/lb.sh up | down | start NAME | stop NAME | pause bN | resume bN | slow bN MS | status | keys [N]
# NAME is front, b1, b2 or b3. pid files and logs live in .lb/.
# Front options come from env: LB_CAP (-B) LB_WAIT (-W) LB_TIMEOUT (-T) LB_THREADS (-t) LB_STRATEGY.
set -u
cd "$(dirname "$0")/.."
DIR=.lb
NAMES="b1 b2 b3 front"
FRONT=http://127.0.0.1:8080
LB_CAP=${LB_CAP:-2} LB_WAIT=${LB_WAIT:-2000} LB_TIMEOUT=${LB_TIMEOUT:-1000}
LB_THREADS=${LB_THREADS:-8} LB_STRATEGY=${LB_STRATEGY:-ketama}

usage() {
    cat <<'EOF'
usage: scripts/lb.sh COMMAND
  up              start front :8080 (/echo) and backends b1..b3 (:8001..:8003)
  down            stop everything
  start NAME      start one of: front b1 b2 b3 (a slow bN is replaced by the webserver)
  stop NAME       stop one of: front b1 b2 b3
  pause bN        freeze one backend (b1, b2 or b3)
  resume bN       continue a paused backend
  slow bN MS      replace bN with tools/slow_backend.py, which answers after MS ms
  status          print the front's options and its /_lb/status table
  keys [N]        send /echo from N source IPs (default 20), show which backend
                  each key hits and how many moved since the last run
front options from env (read by up and start front):
  LB_CAP=2 LB_WAIT=2000 LB_TIMEOUT=1000 LB_THREADS=8 LB_STRATEGY=ketama|rr
  LB_CAP=0 LB_WAIT=0 turns back-pressure off
EOF
    exit 1
}

port() { case $1 in front) echo 8080 ;; *) echo "800${1#b}" ;; esac; }

route_sep() { case $LB_STRATEGY in ketama) echo @ketama= ;; *) echo = ;; esac; }

args() {
    case $1 in
        front) echo "-p 8080 -r www -c cgi-bin -B $LB_CAP -W $LB_WAIT -T $LB_TIMEOUT -t $LB_THREADS" \
                    "-R /echo$(route_sep)127.0.0.1:8001,127.0.0.1:8002,127.0.0.1:8003" ;;
        *) echo "-p $(port "$1") -n $1 -r www" ;;
    esac
}

off() { if [ "$1" = 0 ]; then echo off; else echo "$1${2:-}"; fi; }

save_opts() {
    echo "cap $(off "$LB_CAP")  wait $(off "$LB_WAIT" ms)  timeout ${LB_TIMEOUT}ms  threads $LB_THREADS  $LB_STRATEGY" \
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
        [ "$1" = front ] && save_opts
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

[ $# -ge 1 ] || usage
case $LB_STRATEGY in ketama|rr) ;; *) echo "LB_STRATEGY must be ketama or rr (got '$LB_STRATEGY')"; exit 1 ;; esac
mkdir -p "$DIR"
case $1 in
    up)     make -s all || exit 1
            for n in $NAMES; do start "$n" || exit 1; done ;;
    down)   for n in front b1 b2 b3; do stop "$n"; done; rm -f "$DIR"/*.pid "$DIR"/*.slow ;;
    start)  valid "${2:-}"; make -s all || exit 1; start "$2" ;;
    stop)   valid "${2:-}"; stop "$2" ;;
    pause|resume)
            case ${2:-} in b1|b2|b3) freeze "$1" "$2" ;; *) usage ;; esac ;;
    slow)   case ${2:-} in b1|b2|b3) ;; *) usage ;; esac
            case ${3:-} in ''|*[!0-9]*) usage ;; esac
            stop "$2" >/dev/null; start "$2" "$3" ;;
    status) [ -f "$DIR/front.opts" ] && cat "$DIR/front.opts"
            curl -sf "$FRONT/_lb/status" || { echo "front is not answering /_lb/status"; exit 1; } ;;
    keys)   keys "${2:-20}" ;;
    *)      usage ;;
esac
