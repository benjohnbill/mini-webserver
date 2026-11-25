#!/usr/bin/env bash
# Load-balancer demo: one Ketama front (:8080) and three backends (:8001..:8003).
#   scripts/lb.sh up | down | start NAME | stop NAME | status | keys [N]
# NAME is front, b1, b2 or b3. pid files and logs live in .lb/.
set -u
cd "$(dirname "$0")/.."
DIR=.lb
NAMES="b1 b2 b3 front"
FRONT=http://127.0.0.1:8080

usage() {
    cat <<'EOF'
usage: scripts/lb.sh COMMAND
  up              start front :8080 (/echo@ketama) and backends b1..b3 (:8001..:8003)
  down            stop everything
  start NAME      start one of: front b1 b2 b3
  stop NAME       stop one of: front b1 b2 b3
  status          print the front's /_lb/status table
  keys [N]        send /echo from N source IPs (default 20), show which backend
                  each key hits and how many moved since the last run
EOF
    exit 1
}

port() { case $1 in front) echo 8080 ;; *) echo "800${1#b}" ;; esac; }

args() {
    case $1 in
        front) echo "-p 8080 -r www -c cgi-bin -R /echo@ketama=127.0.0.1:8001,127.0.0.1:8002,127.0.0.1:8003" ;;
        *) echo "-p $(port "$1") -n $1 -r www" ;;
    esac
}

# kill -0 alone counts zombies (parent gone, nobody reaped yet) as alive.
alive() {
    local st
    case $1 in ''|*[!0-9]*) return 1 ;; esac
    st=$(cat "/proc/$1/stat" 2>/dev/null) || return 1
    st=${st##*) }                    # state is the first field after "(comm) "
    [ -n "$st" ] && [ "${st%% *}" != Z ]
}

pid_of() { # prints the pid if NAME is running, else clears a stale pid file
    local f="$DIR/$1.pid" pid
    [ -f "$f" ] || return 1
    pid=$(cat "$f")
    if alive "$pid"; then echo "$pid"; else rm -f "$f"; return 1; fi
}

start() {
    local pid
    if pid=$(pid_of "$1"); then echo "$1 is already running (pid $pid)"; return 0; fi
    # shellcheck disable=SC2046  # args are intentionally word-split
    nohup ./webserver $(args "$1") >"$DIR/$1.log" 2>&1 &
    pid=$!
    echo "$pid" >"$DIR/$1.pid"
    for _ in $(seq 50); do
        if ! alive "$pid"; then echo "$1 failed to start, see $DIR/$1.log"; rm -f "$DIR/$1.pid"; return 1; fi
        curl -s -o /dev/null -m 1 "http://127.0.0.1:$(port "$1")/" && { echo "$1 started (pid $pid, :$(port "$1"))"; return 0; }
        sleep 0.1
    done
    echo "$1 did not accept connections in 5 s, see $DIR/$1.log"; return 1
}

stop() {
    local pid
    if ! pid=$(pid_of "$1"); then echo "$1 is not running"; return 0; fi
    kill -TERM "$pid"
    for _ in $(seq 50); do alive "$pid" || break; sleep 0.1; done
    alive "$pid" && kill -KILL "$pid"
    rm -f "$DIR/$1.pid"
    echo "$1 stopped"
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
        echo "$key $name" >>"$cur"
        prev=$( [ -f "$DIR/keys.prev" ] && awk -v k="$key" '$1==k{print $2}' "$DIR/keys.prev")
        if [ -z "$prev" ]; then
            printf '%-12s %s\n' "$key" "$name"
        else
            cmp=$((cmp + 1))
            if [ "$prev" = "$name" ]; then printf '%-12s %s\n' "$key" "$name"
            else moved=$((moved + 1)); printf '%-12s %s → %s  (moved)\n' "$key" "$prev" "$name"; fi
        fi
    done
    if [ "$cmp" -eq 0 ]; then echo "first run, nothing to compare"
    else echo "moved $moved/$cmp ($((moved * 100 / cmp))%)"; fi
    mv "$cur" "$DIR/keys.prev"
}

valid() { case " $NAMES " in *" ${1:-} "*) ;; *) usage ;; esac; }

[ $# -ge 1 ] || usage
mkdir -p "$DIR"
case $1 in
    up)     make -s all || exit 1
            for n in $NAMES; do start "$n" || exit 1; done ;;
    down)   for n in front b1 b2 b3; do stop "$n"; done; rm -f "$DIR"/*.pid ;;
    start)  valid "${2:-}"; make -s all || exit 1; start "$2" ;;
    stop)   valid "${2:-}"; stop "$2" ;;
    status) curl -sf "$FRONT/_lb/status" || { echo "front is not answering /_lb/status"; exit 1; } ;;
    keys)   keys "${2:-20}" ;;
    *)      usage ;;
esac
