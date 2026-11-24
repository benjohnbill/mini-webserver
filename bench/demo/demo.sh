#!/usr/bin/env bash
# The Act runner of the Back pressure demo (SPEC 12).
#
# Usage: bench/demo/demo.sh act N [dry|rec]   start Act N (1 to 5)
#        bench/demo/demo.sh stop               stop rproxy, the Phones, and the Origin
#
# An Act stops what the last Act left, starts rproxy with the Relay mode of
# the Act inside a memory-limited scope, waits until rproxy answers, writes
# run/act.json (the dashboard resets on it), and starts the Phones. The
# command returns when the Phones start. The Act goes on in the background.
# `rec` also writes scene marks to run/actN.marks.json. `dry` writes none.
#
# Each value is a variable, so a rehearsal can change it:
#   PHONES=100 RATE=300 bench/demo/demo.sh act 4 dry
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DEMO="$ROOT/bench/demo"
RUN="${RUN:-$DEMO/run}"

PHONES="${PHONES:-200}"               # number of Phones
ARRIVAL="${ARRIVAL:-15}"              # seconds to start all Phones
RATE="${RATE:-400}"                   # KB/s of each Phone
FILE_BYTES="${FILE_BYTES:-8000000}"   # size of www/update.bin
FILE_PATH="${FILE_PATH:-/update.bin}"
MEMORY_MAX="${MEMORY_MAX:-1G}"        # memory limit of the rproxy scope
RELAY_CONNS="${RELAY_CONNS:-20}"      # Act 3 limit
RELAY_BUFFER="${RELAY_BUFFER:-65536}" # Act 4 and 5 buffer
ORIGIN_PORT="${ORIGIN_PORT:-19080}"
PROXY_PORT="${PROXY_PORT:-19443}"
RPROXY_ARGS="${RPROXY_ARGS:-}"        # more rproxy flags, for a rehearsal

die() { echo "demo: $*" >&2; exit 1; }

now_ms() { date +%s%3N; }

pid_is_alive() { [[ -n "${1:-}" ]] && kill -0 "$1" 2>/dev/null; }

pid_read() { cat "$RUN/$1.pid" 2>/dev/null || true; }

# Kill the process in run/NAME.pid and wait until it is gone.
pid_kill() {
    local pid
    pid="$(pid_read "$1")"
    if pid_is_alive "$pid"; then
        kill "$pid" 2>/dev/null || true
        for _ in $(seq 50); do pid_is_alive "$pid" || break; sleep 0.1; done
        pid_is_alive "$pid" && kill -9 "$pid" 2>/dev/null || true
    fi
    rm -f "$RUN/$1.pid"
}

# Stop the processes of an Act, also ones that a crashed run left.
act_stop() {
    pid_kill runner
    pid_kill phones
    pid_kill rproxy
    pkill -f "^python3 [^ ]*phones.py --port $PROXY_PORT " 2>/dev/null || true
    pkill -f "^[^ ]*rproxy --port $PROXY_PORT " 2>/dev/null || true
    for _ in $(seq 30); do
        pgrep -f "^[^ ]*rproxy --port $PROXY_PORT " >/dev/null || break
        sleep 0.1
    done
}

origin_stop() {
    pid_kill origin
    pkill -f "^[^ ]*server $ORIGIN_PORT " 2>/dev/null || true
    rm -f "$RUN/origin.model"
}

relay_for_act() {
    case "$1" in
        1) echo unbounded ;;
        2) echo reply-cap ;;
        3) echo conn-cap ;;
        4 | 5) echo backpressure ;;
        *) die "no Act $1 (1 to 5)" ;;
    esac
}

prepare() {
    mkdir -p "$RUN"
    [[ -x "$ROOT/server" && -x "$ROOT/rproxy" ]] || make -C "$ROOT" all
    [[ -f "$ROOT/build/cert.pem" ]] || make -C "$ROOT" cert
    local file="$ROOT/www${FILE_PATH}"
    if [[ ! -f "$file" || "$(stat -c %s "$file")" != "$FILE_BYTES" ]]; then
        head -c "$FILE_BYTES" /dev/urandom >"$file"
    fi
    if ! grep -qx "/www${FILE_PATH}" "$ROOT/.gitignore"; then
        echo "/www${FILE_PATH}" >>"$ROOT/.gitignore"
    fi
}

# Start the Origin once. Act 5 needs a `pool` Origin, the others `epoll`.
origin_start() {
    local model="$1" pid
    pid="$(pid_read origin)"
    if pid_is_alive "$pid" && [[ "$(cat "$RUN/origin.model" 2>/dev/null)" == "$model" ]]; then
        return
    fi
    origin_stop
    (cd "$ROOT" && setsid ./server "$ORIGIN_PORT" www --model "$model" \
        >"$RUN/origin.log" 2>&1 </dev/null & echo $! >"$RUN/origin.pid")
    echo "$model" >"$RUN/origin.model"
    for _ in $(seq 50); do
        curl -s -o /dev/null "http://127.0.0.1:$ORIGIN_PORT/index.html" && return
        sleep 0.1
    done
    die "the Origin does not answer on port $ORIGIN_PORT (see $RUN/origin.log)"
}

# RELAY_FLAGS, when set, replaces the --relay flags (a rehearsal on an
# rproxy with no --relay flag can set it to the empty string).
relay_flags() {
    if [[ -n "${RELAY_FLAGS+set}" ]]; then
        echo "$RELAY_FLAGS"
        return
    fi
    case "$1" in
        conn-cap) echo "--relay conn-cap --relay-conns $RELAY_CONNS" ;;
        backpressure) echo "--relay backpressure --relay-buffer $RELAY_BUFFER" ;;
        *) echo "--relay $1" ;;
    esac
}

# The cgroup of the scope is gone when rproxy is dead, so the OOM counter is
# gone too. systemd writes the cause to the journal: find it there.
oom_check() {
    for _ in $(seq 20); do
        if journalctl --user --no-pager -q --since "-1min" -u "$1" 2>/dev/null \
            | grep -q "result 'oom-kill'"; then
            echo "oom-kill" >"$RUN/rproxy.exit"
            return
        fi
        sleep 0.1
    done
}

# The body of an Act. It runs in the background (see act_start).
act_run() {
    local act="$1" mode="$2" relay origin_model t0 rproxy_pid phones_pid
    local marks=() exit_code cgroup
    relay="$(relay_for_act "$act")"
    origin_model="$(cat "$RUN/origin.model")"
    t0="$(now_ms)"
    mark() {
        marks+=("{\"name\": \"$1\", \"ms\": $(($(now_ms) - t0))}")
        echo "[$(( ($(now_ms) - t0) / 1000 )) s] $1"
    }

    # systemd-run --scope runs rproxy in place, so $! is the pid of rproxy.
    # shellcheck disable=SC2046
    systemd-run --user --scope --quiet -p MemoryMax="$MEMORY_MAX" \
        -p MemorySwapMax=0 "$ROOT/rproxy" --port "$PROXY_PORT" \
        --cert "$ROOT/build/cert.pem" --key "$ROOT/build/key.pem" \
        --origin "127.0.0.1:$ORIGIN_PORT" --cache off \
        $(relay_flags "$relay") $RPROXY_ARGS >"$RUN/rproxy.log" 2>&1 &
    rproxy_pid=$!
    echo "$rproxy_pid" >"$RUN/rproxy.pid"
    for _ in $(seq 100); do
        pid_is_alive "$rproxy_pid" || break
        curl -k -s -o /dev/null --max-time 1 \
            "https://127.0.0.1:$PROXY_PORT/index.html" && break
        sleep 0.1
    done
    if ! pid_is_alive "$rproxy_pid"; then
        echo "rproxy did not start:" && cat "$RUN/rproxy.log"
        echo "failed" >"$RUN/act.failed"
        return 1
    fi
    cgroup="/sys/fs/cgroup$(cut -d: -f3 "/proc/$rproxy_pid/cgroup")"
    cat >"$RUN/act.tmp" <<EOF
{"act": $act, "relay": "$relay", "origin_model": "$origin_model",
 "rproxy_pid": $rproxy_pid, "cgroup": "$cgroup",
 "memory_max": $(numfmt --from=iec "$MEMORY_MAX"),
 "phones": $PHONES, "rate": $RATE, "arrival": $ARRIVAL,
 "mode": "$mode", "started": $(date +%s.%N)}
EOF
    mv "$RUN/act.tmp" "$RUN/act.json"
    mark "start"

    python3 "$DEMO/phones.py" --port "$PROXY_PORT" --count "$PHONES" \
        --arrival "$ARRIVAL" --rate "$RATE" --path "$FILE_PATH" \
        --status "$RUN/status.json" >"$RUN/phones.log" 2>&1 &
    phones_pid=$!
    echo "$phones_pid" >"$RUN/phones.pid"

    # Mark the first of: rproxy stops, the Phones end. Then the other.
    local ended
    while true; do
        set +e
        wait -n -p ended "$rproxy_pid" "$phones_pid"
        exit_code=$?
        set -e
        if [[ "$ended" == "$rproxy_pid" ]]; then
            echo "$exit_code" >"$RUN/rproxy.exit"
            if [[ "$exit_code" == 137 ]]; then
                mark "rproxy killed"
                oom_check "$(basename "$cgroup")"
            else
                mark "rproxy exited $exit_code"
            fi
            rproxy_pid=""
        else
            mark "phones finished"
            phones_pid=""
        fi
        [[ -z "$phones_pid" ]] && break
        [[ -z "$rproxy_pid" ]] && { wait "$phones_pid" || true; mark "phones finished"; break; }
    done
    if [[ "$mode" == rec ]]; then
        local IFS=,
        printf '{"act": %s, "relay": "%s", "t0_ms": %s, "marks": [%s]}\n' \
            "$act" "$relay" "$t0" "${marks[*]}" >"$RUN/act$act.marks.json"
        echo "marks: $RUN/act$act.marks.json"
    fi
    # rproxy stays up after the Phones end, for the bystander curl.
    [[ -n "$rproxy_pid" ]] && wait "$rproxy_pid" || true
}

act_start() {
    local act="$1" mode="${2:-dry}" model=epoll
    [[ "$mode" == dry || "$mode" == rec ]] || die "mode is dry or rec"
    relay_for_act "$act" >/dev/null
    [[ "$act" == 5 ]] && model=pool
    if [[ -z "${RELAY_FLAGS+set}" ]]; then
        local help
        help="$("$ROOT/rproxy" --help 2>&1 || true)"
        [[ "$help" == *--relay* ]] || die "this rproxy has no --relay flag; rebuild it (make rproxy)"
    fi
    prepare
    act_stop
    origin_start "$model"
    rm -f "$RUN/status.json" "$RUN/rproxy.exit" "$RUN/act.failed" \
        "$RUN/act.json"
    setsid bash "$DEMO/demo.sh" _run "$act" "$mode" \
        >"$RUN/runner.log" 2>&1 </dev/null &
    echo $! >"$RUN/runner.pid"
    for _ in $(seq 100); do
        [[ -f "$RUN/act.failed" ]] && { cat "$RUN/runner.log"; die "Act $act failed"; }
        [[ -f "$RUN/act.json" ]] && break
        sleep 0.1
    done
    [[ -f "$RUN/act.json" ]] || die "rproxy did not answer in 10 s (see $RUN/runner.log)"
    echo "Act $act: rproxy --relay $(relay_for_act "$act"), Origin $model," \
        "$PHONES Phones at $RATE KB/s ($mode)"
}

case "${1:-}" in
    act) [[ -n "${2:-}" ]] || die "usage: demo.sh act N [dry|rec]"
         act_start "$2" "${3:-dry}" ;;
    _run) act_run "$2" "$3" ;;
    stop) act_stop; origin_stop; echo "stopped" ;;
    *) sed -n '2,13p' "$0"; exit 2 ;;
esac
