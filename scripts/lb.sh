#!/usr/bin/env bash
# Load-balancer demo: one Ketama front (:8080) and backends bN on :8000+N (default b1..b3).
#   scripts/lb.sh up | down | add | remove bN | start NAME | stop NAME | pause bN | resume bN
#                 | slow bN MS | status | keys [N] | weight bN W | client new N [--rps R] | client rm N
#                 | client ls | client stop
# NAME is front or a member backend. The member list is .lb/backends, weights other than 1 are in
# .lb/weights ("bN W"), the front reads its /echo backends from .lb/front.route (add/remove/weight
# rewrite it and send SIGHUP); pid files and logs live in .lb/.
# Front options come from env: LB_CAP (-B) LB_WAIT (-W) LB_TIMEOUT (-T) LB_THREADS (-t) LB_STRATEGY
# LB_BACKENDS LB_WEIGHTS.
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
# Weights are 1..4, like the front's *N. A bad one is refused before any file or the front changes.
valid_weight() { case ${1:-} in [1-4]) ;; *) echo "weight must be 1..4 (got '${1:-}')" >&2; return 1 ;; esac; }
split_commas() { awk -F, '{for (i = 1; i <= NF; i++) print $i}' <<<"$1"; }
# The setup a fresh up takes from LB_BACKENDS / LB_WEIGHTS; both must be one line (no newline, no empty item).
defaults() {
    local n=${LB_BACKENDS:-} c
    [[ -z $n || $n =~ ^[0-9]{1,2}$ ]] || { echo "LB_BACKENDS must be 1..$MAXN (got '$n')" >&2; exit 1; }
    if [ -n "${LB_WEIGHTS:-}" ]; then
        [[ $LB_WEIGHTS =~ ^[1-4](,[1-4])*$ ]] ||
            { echo "LB_WEIGHTS must be one line of weights 1..4 such as 1,3,1 (got '$LB_WEIGHTS')" >&2; exit 1; }
        c=$(split_commas "$LB_WEIGHTS" | wc -l)
        n=${n:-$c}
        [ "$((10#$n))" = "$c" ] ||
            { echo "LB_WEIGHTS has $c values but LB_BACKENDS is $n" >&2; exit 1; }
    fi
    n=${n:-3}
    [ "$((10#$n))" -ge 1 ] && [ "$((10#$n))" -le "$MAXN" ] ||
        { echo "LB_BACKENDS must be 1..$MAXN (got '$n')" >&2; exit 1; }
    seq "$((10#$n))" | sed 's/^/b/' | sorted
}
# Options a newly started front gets (same ranges as src/config.c), checked before an old front is stopped.
front_opts_ok() {
    local spec name lo hi
    for spec in "LB_CAP 0 1024" "LB_WAIT 0 600000" "LB_TIMEOUT 1 600000" "LB_THREADS 1 256"; do
        read -r name lo hi <<<"$spec"
        [[ ${!name} =~ ^[0-9]{1,6}$ ]] && [ "$((10#${!name}))" -ge "$lo" ] && [ "$((10#${!name}))" -le "$hi" ] ||
            { echo "$name must be $lo..$hi (got '${!name}')"; return 1; }
    done
    case $LB_STRATEGY in ketama|rr) ;; *) echo "LB_STRATEGY must be ketama or rr (got '$LB_STRATEGY')"; return 1 ;; esac
}
# "b2:3 b5:2": members whose weight is not 1 (.lb/weights holds the same as "bN W" lines).
default_weights() {
    [ -n "${LB_WEIGHTS:-}" ] || return 0
    split_commas "$LB_WEIGHTS" | awk '$1 != 1 {printf "b%d:%s ", NR, $1}'
}
weight_of() { local e; for e in $WEIGHTS; do [ "${e%%:*}" = "$1" ] && { echo "${e#*:}"; return; }; done; echo 1; }
set_weight() { # NAME W (W 1 drops the entry)
    local e out=
    for e in $WEIGHTS; do [ "${e%%:*}" = "$1" ] || out+="$e "; done
    [ "$2" = 1 ] || out+="$1:$2 "
    WEIGHTS=$out
}
weight_list() { local b out=; for b in $BACKENDS; do out+="${out:+,}$(weight_of "$b")"; done; echo "$out"; }
# defaults exits only its $(...) subshell, so every caller checks it. Here a bad LB_BACKENDS/LB_WEIGHTS
# must not block down, stop or client: only a fresh up (below) insists on them.
if [ -s "$MEMBERS" ]; then BACKENDS=$(sorted <"$MEMBERS")
else BACKENDS=$(defaults 2>/dev/null) || BACKENDS=$(LB_BACKENDS='' LB_WEIGHTS='' defaults); fi
WFILE=$DIR/weights
WEIGHTS=$( [ -f "$WFILE" ] && awk '$2 ~ /^[1-4]$/ && $2 != 1 {printf "%s:%s ", $1, $2}' "$WFILE")
NAMES="${BACKENDS}front"
ROUTE=$DIR/front.route   # the front's /echo backend list, one host:port[*W] per line; re-read on SIGHUP
save_members() {
    NAMES="${BACKENDS}front"
    route_lines | put "$ROUTE" && weight_lines | put "$WFILE" && echo "$BACKENDS" | put "$MEMBERS" ||
        { echo "could not write $ROUTE, $WFILE or $MEMBERS"; return 1; }
}
addrs() { for b in $BACKENDS; do echo "127.0.0.1:$(port "$b")"; done; }
route_lines() { local b w; for b in $BACKENDS; do w=$(weight_of "$b"); echo "127.0.0.1:$(port "$b")$([ "$w" = 1 ] || echo "*$w")"; done; }
weight_lines() { local b w; for b in $BACKENDS; do w=$(weight_of "$b"); [ "$w" = 1 ] || echo "$b $w"; done; }
# put FILE: stdin into FILE by tmp + mv, so the front never reads a half-written file. A file that already
# holds that text is left alone, so a rollback only has to rewrite what the failed change actually wrote.
put() {
    local text
    text=$(cat)
    [ -f "$1" ] && [ "$(cat "$1")" = "$text" ] && return 0
    { [ -z "$text" ] || printf '%s\n' "$text"; } >"$1.tmp" && mv -f "$1.tmp" "$1"
}
now_ms() { date +%s%3N; }

usage() {
    cat <<'EOF'
usage: scripts/lb.sh COMMAND
  up              start front :8080 (/echo) and backends b1..bN (:8001..:800N)
  down            stop everything and forget added backends
  add [--weight W]
                  start a backend on the lowest free bN (:8000+N) and reload
                  the front's backend list (SIGHUP) so it routes there too (weight W 1..4, default 1)
  remove bN       drop bN from the front's route (SIGHUP reload), then stop bN
  weight bN W     set bN's weight (1..4) and reload the front
  client new N [--rps R] | client rm N | client ls | client stop
                  N more / fewer steady clients (tools/clientd.py, source IPs 127.0.0.2.., at most 250),
                  each sending one /echo at a time, R per second (default 2)
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
  LB_BACKENDS=3   backends that up starts (1..ROUTE_MAX_BACKENDS, 8) when there is no member list yet
                  (first up, or after down); otherwise up keeps .lb/backends and its weights: use add / remove
  LB_WEIGHTS=1,3,1  their weights (1..4 each); without LB_BACKENDS the count is the number of weights
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
    echo "cap $(off "$LB_CAP")  wait $(off "$LB_WAIT" ms)  timeout ${LB_TIMEOUT}ms  threads $LB_THREADS  $LB_STRATEGY  backends $(echo $BACKENDS | wc -w)  weights $(weight_list)" \
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
        [ "$1" = front ] && { save_opts; route_lines | put "$ROUTE"; }
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
    local n=${1:-20} i key name prev moved=0 cmp=0 cur hits= b k total
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
        hits+="$name "
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
    # Per member: how many of this run's answered keys it took, next to its weight.
    total=$(echo $hits | wc -w)
    for b in $BACKENDS; do
        k=$(tr ' ' '\n' <<<"$hits" | grep -cx "$b")
        printf '%s  weight %s  keys %s (%s%%)\n' "$b" "$(weight_of "$b")" "$k" "$((total ? k * 100 / total : 0))"
    done
    mv "$cur" "$DIR/keys.prev"
}

valid() { case " $NAMES " in *" ${1:-} "*) ;; *) usage ;; esac; }
backend() { case " $BACKENDS " in *" ${1:-} "*) ;; *) echo "${1:-?} is not a backend (members: $BACKENDS)"; exit 1 ;; esac; }

# Old way: a front started with the list inline (or an old binary) only knows its start-up route.
refront() {
    pid_of front >/dev/null || return 0
    # Refuse before stopping the old front: a new one that cannot start would leave no front at all.
    front_opts_ok || return 1
    stop front >/dev/null
    start front
}

# shows_list: /_lb/status lists BACKENDS with their weights (address and WEIGHT column) within 2 s.
shows_list() {
    local want end left
    want=$(for b in $BACKENDS; do echo "127.0.0.1:$(port "$b") $(weight_of "$b")"; done)
    end=$(( $(now_ms) + 2000 ))
    while left=$(( end - $(now_ms) )); [ "$left" -gt 0 ]; do
        # Every status request gets only the time left, so the whole wait stays within 2 s.
        [ "$(curl -s -m "$(printf '%d.%03d' $((left / 1000)) $((left % 1000)))" "$FRONT/_lb/status" |
             awk '$1 ~ /^127\.0\.0\.1:/ {print $1, $3}')" = "$want" ] && return 0
        [ "$left" -gt 100 ] && sleep 0.1
    done
    return 1
}

# The route file is already rewritten; tell the front to re-read it and wait until /_lb/status shows it.
reload() {
    local pid
    pid=$(pid_of front) || return 0
    # An old front dies on SIGHUP; only a front started on the file (-R ...=@file) gets one.
    tr '\0' ' ' <"/proc/$pid/cmdline" 2>/dev/null | grep -q '=@' || { refront; return; }
    kill -HUP "$pid" || { echo "could not send SIGHUP to the front (pid $pid)"; return 1; }
    shows_list || { echo "front did not show the new backend list in 2 s, see $DIR/front.log"; return 1; }
    echo "front reloaded (pid $pid, :8080, webserver)"
    # Only the members changed: keep the options the running front was started with.
    [ -f "$DIR/front.opts" ] &&
        sed -i "s/backends [0-9]*.*$/backends $(echo $BACKENDS | wc -w)  weights $(weight_list)/" "$DIR/front.opts"
    return 0
}

# BACKENDS/WEIGHTS are already the new ones. Write them and reload; on any failure put the OLD
# ones ($1, $2) back in the files and in the front, so nothing changed and the command can be retried.
# The rollback itself is checked: if the front ran before, it must run and show the old list again.
apply() {
    local was_up=
    pid_of front >/dev/null && was_up=1
    save_members && reload && return 0
    BACKENDS=$1 WEIGHTS=$2
    save_members >/dev/null ||
        { echo "rollback failed: could not write the old list back to $ROUTE, $WFILE or $MEMBERS"; return 1; }
    reload >/dev/null
    if [ -n "$was_up" ]; then
        pid_of front >/dev/null ||
            { echo "rollback failed: the front is not running (files hold backends $BACKENDS, weights $(weight_list)); start it with: lb.sh start front"; return 1; }
        shows_list ||
            { echo "rollback failed: the front does not show the old list in 2 s (files hold backends $BACKENDS, weights $(weight_list)), see $DIR/front.log"; return 1; }
    fi
    echo "member change not applied; backends stay: $BACKENDS (weights $(weight_list))"
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

add() { # [W]
    local n name w=${1:-1}
    [ "$(echo $BACKENDS | wc -w)" -lt "$MAXN" ] ||
        { echo "the front takes at most $MAXN backends (ROUTE_MAX_BACKENDS); remove one first"; return 1; }
    for n in $(seq $MAXB); do
        name=b$n
        case " $BACKENDS " in *" $name "*) continue ;; esac
        accepts "$name" && continue          # someone else already listens on :800N
        start "$name" || return 1
        local old=$BACKENDS oldw=$WEIGHTS
        BACKENDS=$(echo "$BACKENDS $name" | sorted)
        set_weight "$name" "$w"
        apply "$old" "$oldw" || { stop "$name"; return 1; }
        echo "backends: $BACKENDS"
        return 0
    done
    echo "no free backend slot (b1..b$MAXB, :8001..:80$MAXB)"; return 1
}

remove() {
    backend "$1"
    [ "$(echo $BACKENDS | wc -w)" -gt 1 ] || { echo "$1 is the last backend; use down instead"; return 1; }
    local old=$BACKENDS oldw=$WEIGHTS
    BACKENDS=$(for b in $BACKENDS; do [ "$b" = "$1" ] || echo "$b"; done | sorted)
    set_weight "$1" 1
    # Out of the route first, so the front sends it nothing new; stop only once that took effect.
    apply "$old" "$oldw" || return 1
    drain "$1"
    stop "$1"
    echo "backends: $BACKENDS"
}

weight() { # bN W, already validated
    local oldw=$WEIGHTS
    set_weight "$1" "$2"
    apply "$BACKENDS" "$oldw" || return 1
    echo "weights: $(for b in $BACKENDS; do printf '%s=%s ' "$b" "$(weight_of "$b")"; done)"
}

# Steady clients: one tools/clientd.py daemon; .lb/clients holds "count N" and "rps R", re-read by it.
CSTATE=$DIR/clients CSTATS=$DIR/clients.stats CMAX=250
cstate() { [ -f "$CSTATE" ] && awk -v k="$1" '$1 == k {print $2}' "$CSTATE"; }
valid_rps() { awk -v r="$1" 'BEGIN {exit !(r ~ /^[0-9]+(\.[0-9]+)?$/ && r + 0 >= 0.1 && r + 0 <= 20)}' ||
    { echo "rps must be 0.1..20 (got '$1')"; return 1; }; }
client() {
    local n count rps pid
    count=$(cstate count) rps=$(cstate rps)
    count=${count:-0} rps=${rps:-2}
    case ${1:-} in
        new|rm)
            n=${2:-}
            case $n in ''|*[!0-9]*) echo "usage: lb.sh client new N [--rps R] | client rm N"; return 1 ;; esac
            # Size first: a 20-digit N would wrap around in shell arithmetic. Past 4 digits, new is
            # over the limit anyway and rm takes everything away.
            n=${n#"${n%%[1-9]*}"}
            if [ "${#n}" -gt 4 ]; then n=99999; else n=$((10#${n:-0})); fi
            # No daemon (never started, or it died behind our back): no clients run, so count from 0.
            pid_of clients >/dev/null || count=0
            if [ "$1" = new ]; then
                case ${3:-} in
                    '') ;;
                    --rps) valid_rps "${4:-}" || return 1; rps=${4} ;;
                    *) echo "usage: lb.sh client new N [--rps R]"; return 1 ;;
                esac
                [ $((count + n)) -le "$CMAX" ] ||
                    { echo "at most $CMAX clients (source IPs 127.0.0.2..251); now $count, asked for $2 more"; return 1; }
                count=$((count + n))
            else
                [ $# -le 2 ] || { echo "usage: lb.sh client rm N"; return 1; }
                count=$((count > n ? count - n : 0))
            fi
            local prev
            prev=$(cat "$CSTATE" 2>/dev/null)
            printf 'count %s\nrps %s\n' "$count" "$rps" >"$CSTATE.tmp" && mv -f "$CSTATE.tmp" "$CSTATE" ||
                { echo "could not write $CSTATE"; return 1; }
            if [ "$1" = new ] && ! pid_of clients >/dev/null; then
                rm -f "$CSTATS" "$CSTATS.tmp"         # stale ones would pass for the new daemon's
                # 9>&-: the daemon must not inherit the clients.lock descriptor and hold the lock forever.
                nohup python3 tools/clientd.py --state "$CSTATE" --stats "$CSTATS" >"$DIR/clients.log" 2>&1 9>&- &
                pid=$!
                echo "$pid" >"$DIR/clients.pid"
                # Up means its first stats file appeared; give it 1 s, and a daemon that died meanwhile fails.
                for _ in $(seq 10); do alive "$pid" || break; [ -f "$CSTATS" ] && break; sleep 0.1; done
                if ! alive "$pid"; then
                    # No daemon took the new count: put the state back as it was.
                    rm -f "$DIR/clients.pid"
                    if [ -n "$prev" ]; then printf '%s\n' "$prev" >"$CSTATE"; else rm -f "$CSTATE"; fi
                    echo "client daemon failed to start, see $DIR/clients.log"
                    return 1
                fi
            fi
            echo "clients: $count (rps $rps)"
            awk -v c="$count" -v r="$rps" 'BEGIN {if (c * r > 200) printf "warning: %d clients x %s rps = %.0f req/s may saturate the front\n", c, r, c * r}' ;;
        ls)
            [ $# -eq 1 ] || { echo "usage: lb.sh client ls"; return 1; }
            if ! pid_of clients >/dev/null; then echo "no clients running"; return 0; fi
            cat "$CSTATS" 2>/dev/null || echo "clients: $count (rps $rps), no stats yet" ;;
        stop)
            [ $# -eq 1 ] || { echo "usage: lb.sh client stop"; return 1; }
            stop clients
            rm -f "$CSTATE" "$CSTATE.tmp" "$CSTATS" "$CSTATS.tmp" ;;
        *)  echo "usage: lb.sh client new N [--rps R] | rm N | ls | stop"; return 1 ;;
    esac
}

# One client command at a time (state read + write, daemon start/stop), also against down.
client_locked() { ( flock 9 || { echo "could not lock $DIR/clients.lock"; exit 1; }; client "$@" ) 9>"$DIR/clients.lock"; }
# Is the front running? Unlike pid_of, leaves a stale pid file alone (up decides before any side effect).
front_running() { [ -f "$DIR/front.pid" ] && alive "$(cat "$DIR/front.pid")"; }

[ $# -ge 1 ] || usage
mkdir -p "$DIR"
case $1 in
    up)     # No member list (first up, or after down): a fresh setup from LB_BACKENDS / LB_WEIGHTS,
            # checked before make or any file changes. A member list (front running or only stopped,
            # e.g. a restarted lab container) keeps its members and weights (added ones too).
            fresh=
            if [ ! -s "$MEMBERS" ]; then
                BACKENDS=$(defaults) || exit 1
                WEIGHTS=$(default_weights)
                fresh=1
            elif [ -n "${LB_BACKENDS:-}${LB_WEIGHTS:-}" ]; then
                echo "keeping members from .lb/backends (lb.sh down to reset)" >&2
            fi
            was_up=
            if front_running; then was_up=1; else front_opts_ok || exit 1; fi
            make -s all || exit 1
            [ -z "$fresh" ] || save_members || exit 1
            for n in $NAMES; do start "$n" || exit 1; done
            # A front already running (down, then only start front) still has its old route: reload it
            # onto the fresh list once the backends are up.
            [ -z "$fresh" ] || [ -z "$was_up" ] || reload || exit 1 ;;
    # down stops the clients, then every bN with a pid file, members or not, and forgets the member list.
    down)   client_locked stop >/dev/null
            stop front
            for f in "$DIR"/b*.pid; do [ -e "$f" ] && stop "$(basename "$f" .pid)"; done
            # Not clients.pid: that one belongs to client_locked, and a client new may have started since.
            rm -f "$DIR"/front.pid "$DIR"/b*.pid "$DIR"/*.slow "$MEMBERS" "$ROUTE" "$WFILE" ;;
    add)    w=1
            case ${2:-} in
                '') [ $# -eq 1 ] || usage ;;
                --weight) [ $# -eq 3 ] || usage; valid_weight "$3" || exit 1; w=$3 ;;
                *) usage ;;
            esac
            make -s all || exit 1; add "$w" ;;
    weight) backend "${2:-}"; [ $# -eq 3 ] || usage; valid_weight "$3" || exit 1
            make -s all || exit 1; weight "$2" "$3" ;;
    client) shift; client_locked "$@" ;;
    # Build first: an old front is restarted (refront) on the new binary, which knows -R ...=@file.
    remove) backend "${2:-}"; make -s all || exit 1; remove "$2" ;;
    start)  valid "${2:-}"; [ "$2" != front ] || front_opts_ok || exit 1; make -s all || exit 1; start "$2" ;;
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
