#!/usr/bin/env bash
# The Act runner of the Back pressure demo (SPEC 12).
#
# Usage: bench/demo/demo.sh act N [dry|rec]   start Act N (1 to 5)
#        bench/demo/demo.sh present            the live flow, one Phase for each Enter
#        bench/demo/demo.sh stop               stop rproxy, the Phones, and the Origin
#
# An Act stops what the last Act left, starts rproxy with the Relay mode of
# the Act inside a memory-limited scope, waits until rproxy answers, writes
# run/act.json (the dashboard resets on it), and starts the Phones: one
# phones.py process for each speed tier of MIX. The command returns when the
# Phones start. The Act goes on in the background. It ends when all Phone
# processes end or rproxy dies. `rec` also writes scene marks to
# run/actN.marks.json. `dry` writes none.
#
# Each value is a variable, so a rehearsal can change it:
#   MIX="fast:50000:100 1mbps:125:20" PHONE_RATE=100 bench/demo/demo.sh act 4 dry
#   PRESENT_PAUSE=20 bench/demo/demo.sh present    # no keyboard: sleep 20 s
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DEMO="$ROOT/bench/demo"
RUN="${RUN:-$DEMO/run}"

# The speed tiers of the Phones: label:KB/s:count. The 400 kbps, 1 Mbps,
# 3 Mbps, and 5 Mbps tiers are the speed caps that Korean carriers put on a
# phone after its data allowance (QoS). The share of each tier is an
# assumption, not a measurement. FINAL-10k: most Phones are slow.
# SPEED_SCALE divides every speed: the Python Phones of one laptop cannot
# read 10k Phones at the real speeds (they took 100 % CPU at about 1.3 GB/s,
# 2026-11-26). The ratios between the tiers stay.
SPEED_SCALE="${SPEED_SCALE:-10}"
MIX="${MIX:-fast:50000:500 5mbps:625:500 3mbps:375:1000 1mbps:125:3000 400kbps:50:5000}"
# Phones per second that start, for all tiers together. Act 1 is slower, so
# that the memory line has time to climb before the kill.
PHONE_RATE="${PHONE_RATE:-500}"
PHONE_RATE_1="${PHONE_RATE_1:-200}"
PHONES_PER_PROC="${PHONES_PER_PROC:-1000}" # one phones.py has at most this many
FILE_BYTES="${FILE_BYTES:-1000000}"   # size of www/update.bin
FILE_PATH="${FILE_PATH:-/update.bin}"
MEMORY_MAX="${MEMORY_MAX:-4G}"        # memory limit of the rproxy scope (Q35)
CONNECTIONS="${CONNECTIONS:-12000}"   # rproxy --connections
# Act 2 limit, MiB of replies. With 2560 the base cost of the connections
# came on top, and rproxy died at 4G anyway (2026-11-26).
RELAY_MEMORY="${RELAY_MEMORY:-2048}"
RELAY_CONNS="${RELAY_CONNS:-2000}"    # Act 3 limit
RELAY_QUEUE="${RELAY_QUEUE:-60}"      # Act 3: seconds in the queue, then 503
RELAY_BUFFER="${RELAY_BUFFER:-16384}" # Act 4 and 5 buffer
# The Origin: epoll-pool, ORIGIN_WORK ms of CPU for each answer (the Handler
# time). Act 5 is the same load on a pool Origin, for the comparison.
ORIGIN_MODEL="${ORIGIN_MODEL:-epoll-pool}"
ORIGIN_WORKERS="${ORIGIN_WORKERS:-8}"
ORIGIN_WORK="${ORIGIN_WORK:-5}"
CERT="${CERT:-ec}"                    # ec (ECDSA P-256) or rsa
# rproxy waits this long for a client. The 5 s default cut the Phones under
# about 20 KB/s. 60 s is the default of nginx send_timeout.
CLIENT_TIMEOUT="${CLIENT_TIMEOUT:-60}"
# rproxy waits this long for the first Origin bytes. Under the 10k load the
# Origin Workers share the CPUs with rproxy, and 5 s gave 504s. 60 s is the
# default of nginx proxy_read_timeout.
PROXY_ORIGIN_TIMEOUT="${PROXY_ORIGIN_TIMEOUT:-60}"
# The kernel send buffer of each Origin socket. With autotuning the Origin
# puts a whole 1 MB reply into the kernel and closes at once.
ORIGIN_SEND_BUFFER="${ORIGIN_SEND_BUFFER:-16384}"
# The Origin closes a connection after this many seconds with no progress.
# In Act 4 the Origin sends only as fast as a slow Phone reads, so its send
# queue drains slowly and the 5 s default cuts the 1 Mbps Phones. 60 s is
# the default of nginx send_timeout.
ORIGIN_TIMEOUT="${ORIGIN_TIMEOUT:-60}"
ORIGIN_PORT="${ORIGIN_PORT:-19080}"
PROXY_PORT="${PROXY_PORT:-19443}"
RPROXY_ARGS="${RPROXY_ARGS:-}"        # more rproxy flags, for a rehearsal
PRESENT_PAUSE="${PRESENT_PAUSE:-}"    # seconds; replaces each Enter of `present`
PRESENT_DEATH_WAIT=60                 # Phase 1: the longest wait for the kill
PRESENT_ACT4_WAIT=10                  # Phase 3: seconds before the bystander
PRESENT_2A_WAIT="${PRESENT_2A_WAIT:-12}"   # Phase 2-A: seconds of reply-cap
# Phase 2-A: seconds before the bystander. Early, so that the dashboard shows
# "/index.html 200" next to the 502s for the rest of 2-A.
PRESENT_2A_BYSTANDER="${PRESENT_2A_BYSTANDER:-4}"
PRESENT_2B_WAIT="${PRESENT_2B_WAIT:-30}"   # Phase 2-B: seconds of conn-cap (measured: 503s pile up after 15-30 s)

# The Phones get the upper half of the CPUs, rproxy and the Origin the lower
# half, so the Python clients do not take CPU time from the servers.
CPU_COUNT="$(nproc)"
CPU_HALF=$((CPU_COUNT / 2 > 0 ? CPU_COUNT / 2 : 1))
SERVER_CPUS="${SERVER_CPUS:-0-$((CPU_HALF - 1))}"
PHONE_CPUS="${PHONE_CPUS:-$((CPU_COUNT > 1 ? CPU_HALF : 0))-$((CPU_COUNT - 1))}"

die() { echo "demo: $*" >&2; exit 1; }

# 10k Phones need two sockets each in rproxy. Raise the soft limit to the
# hard limit.
NOFILE_NEED=$((CONNECTIONS * 2 + 1000))
ulimit -n "$(ulimit -Hn)" 2>/dev/null || true
[[ "$(ulimit -n)" == unlimited || "$(ulimit -n)" -ge "$NOFILE_NEED" ]] \
    || die "ulimit -n is $(ulimit -n) (hard $(ulimit -Hn)); the demo needs $NOFILE_NEED"

now_ms() { date +%s%3N; }

pid_is_alive() { [[ -n "${1:-}" ]] && kill -0 "$1" 2>/dev/null; }

pid_read() { cat "$RUN/$1.pid" 2>/dev/null || true; }

# Kill the processes in run/NAME.pid (one pid on each line) and wait until
# they are gone.
pid_kill() {
    local pid
    for pid in $(pid_read "$1"); do
        if pid_is_alive "$pid"; then
            kill "$pid" 2>/dev/null || true
            for _ in $(seq 50); do pid_is_alive "$pid" || break; sleep 0.1; done
            pid_is_alive "$pid" && kill -9 "$pid" 2>/dev/null || true
        fi
    done
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
    rm -f "$RUN/origin.model" "$RUN/origin.key"
}

relay_for_act() {
    case "$1" in
        1) echo unbounded ;;
        2) echo memory-cap ;;
        3) echo conn-cap ;;
        4 | 5) echo backpressure ;;
        *) die "no Act $1 (1 to 5)" ;;
    esac
}

# Print the tiers of MIX as "label rate count" lines.
mix_tiers() {
    local tier label rate count
    for tier in $MIX; do
        IFS=: read -r label rate count <<<"$tier"
        [[ "$label" =~ ^[A-Za-z0-9_.-]+$ && "$rate" =~ ^[0-9]+(\.[0-9]+)?$ \
            && "$count" =~ ^[0-9]+$ ]] \
            || die "bad tier '$tier' in MIX (label:KB/s:count)"
        echo "$label $(python3 -c "print($rate / $SPEED_SCALE)") $count"
    done
}

mix_total() {
    local total=0 label rate count
    while read -r label rate count; do
        total=$((total + count))
    done < <(mix_tiers)
    echo "$total"
}

mix_json() {
    local items=() label rate count
    while read -r label rate count; do
        items+=("{\"label\": \"$label\", \"rate\": $rate, \"count\": $count}")
    done < <(mix_tiers)
    local IFS=,
    echo "[${items[*]}]"
}

prepare() {
    mkdir -p "$RUN"
    [[ -x "$ROOT/server" && -x "$ROOT/rproxy" ]] || make -C "$ROOT" all
    [[ -f "$ROOT/build/cert.pem" ]] || make -C "$ROOT" cert
    [[ -f "$ROOT/build/cert-ec.pem" ]] || openssl req -x509 -newkey ec \
        -pkeyopt ec_paramgen_curve:P-256 -nodes -days 365 -subj /CN=localhost \
        -addext subjectAltName=DNS:localhost,IP:127.0.0.1 \
        -keyout "$ROOT/build/key-ec.pem" -out "$ROOT/build/cert-ec.pem" 2>/dev/null
    local file="$ROOT/www${FILE_PATH}"
    if [[ ! -f "$file" || "$(stat -c %s "$file")" != "$FILE_BYTES" ]]; then
        head -c "$FILE_BYTES" /dev/urandom >"$file"
    fi
    if ! grep -qx "/www${FILE_PATH}" "$ROOT/.gitignore"; then
        echo "/www${FILE_PATH}" >>"$ROOT/.gitignore"
    fi
}

# Start the Origin once. Act 5 needs a `pool` Origin, the others
# ORIGIN_MODEL. A change of a value starts a new Origin.
origin_start() {
    local model="$1" key pid
    key="$model $ORIGIN_TIMEOUT $SERVER_CPUS $ORIGIN_WORKERS $ORIGIN_WORK $ORIGIN_SEND_BUFFER"
    pid="$(pid_read origin)"
    if pid_is_alive "$pid" && [[ "$(cat "$RUN/origin.key" 2>/dev/null)" == "$key" ]]; then
        return
    fi
    origin_stop
    (cd "$ROOT" && setsid taskset -c "$SERVER_CPUS" ./server "$ORIGIN_PORT" \
        www --model "$model" --timeout "$ORIGIN_TIMEOUT" \
        --workers "$ORIGIN_WORKERS" --work "$ORIGIN_WORK" \
        --send-buffer "$ORIGIN_SEND_BUFFER" \
        --stats "$RUN/origin.stats" \
        >"$RUN/origin.log" 2>&1 </dev/null & echo $! >"$RUN/origin.pid")
    echo "$model" >"$RUN/origin.model"
    echo "$key" >"$RUN/origin.key"
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
        conn-cap) echo "--relay conn-cap --relay-conns $RELAY_CONNS --relay-queue $RELAY_QUEUE" ;;
        memory-cap) echo "--relay memory-cap --relay-memory $RELAY_MEMORY" ;;
        backpressure) echo "--relay backpressure --relay-buffer $RELAY_BUFFER" ;;
        *) echo "--relay $1" ;;
    esac
}

# The per-connection buffer stats for the dashboard, if this rproxy has them.
relay_stats_flags() {
    if [[ "$("$ROOT/rproxy" --help 2>&1 || true)" == *--relay-stats* ]]; then
        echo "--relay-stats $RUN/relay"
    fi
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

cert_files() {
    case "$CERT" in
        ec) echo "--cert $ROOT/build/cert-ec.pem --key $ROOT/build/key-ec.pem" ;;
        rsa) echo "--cert $ROOT/build/cert.pem --key $ROOT/build/key.pem" ;;
        *) die "CERT is ec or rsa, not $CERT" ;;
    esac
}

# Phones per second of Act $1.
phone_rate() {
    [[ "$1" == 1 ]] && echo "$PHONE_RATE_1" || echo "$PHONE_RATE"
}

# Seconds to start all Phones of Act $1.
arrival_for_act() {
    python3 -c "print(round($(mix_total) / $(phone_rate "$1"), 2))"
}

# Write run/act.json. The dashboard resets when its "started" changes.
# $6 is "true" while `present` holds the Phones (Phase 0).
act_json_write() {
    local act="$1" relay="$2" mode="$3" rproxy_pid="$4" cgroup="$5"
    local held="${6:-false}" arrival
    arrival="$(arrival_for_act "$act")"
    cat >"$RUN/act.tmp" <<EOF
{"act": $act, "relay": "$relay", "origin_model": "$(cat "$RUN/origin.model")",
 "rproxy_pid": $rproxy_pid, "cgroup": "$cgroup",
 "memory_max": $(numfmt --from=iec "$MEMORY_MAX"),
 "phones": $(mix_total), "tiers": $(mix_json), "arrival": $arrival,
 "phone_rate": $(phone_rate "$act"),
 "file_bytes": $FILE_BYTES, "proxy_port": $PROXY_PORT,
 "origin_port": $ORIGIN_PORT, "relay_conns": $RELAY_CONNS,
 "relay_memory": $RELAY_MEMORY, "relay_queue": $RELAY_QUEUE,
 "relay_buffer": $RELAY_BUFFER, "origin_workers": $ORIGIN_WORKERS,
 "origin_work": $ORIGIN_WORK, "cert": "$CERT",
 "speed_scale": $SPEED_SCALE, "client_timeout": $CLIENT_TIMEOUT,
 "mode": "$mode", "held": $held, "started": $(date +%s.%N)}
EOF
    mv "$RUN/act.tmp" "$RUN/act.json"
}

# Start phones.py processes, as children of this shell (so `wait` sees
# them), and add their pids to the array that $1 names. A tier of more than
# PHONES_PER_PROC Phones gets several processes. All processes start their
# Phones over the same arrival time, so the rate of each tier stays even.
phones_start() {
    local -n pids="$1"
    local arrival="$2" tiers tier label rate count part left index
    mapfile -t tiers < <(mix_tiers)
    for tier in "${tiers[@]}"; do
        read -r label rate count <<<"$tier"
        left="$count"
        index=0
        while ((left > 0)); do
            part=$((left < PHONES_PER_PROC ? left : PHONES_PER_PROC))
            taskset -c "$PHONE_CPUS" python3 "$DEMO/phones.py" \
                --port "$PROXY_PORT" --count "$part" --arrival "$arrival" \
                --rate "$rate" --tier "$label" --path "$FILE_PATH" \
                --status "$RUN/status-$label-$index.json" \
                >>"$RUN/phones.log" 2>&1 &
            pids+=("$!")
            left=$((left - part))
            index=$((index + 1))
        done
    done
}

# The body of an Act. It runs in the background (see act_start). With
# `hold`, it waits for run/go before it starts the Phones (see `present`).
act_run() {
    local act="$1" mode="$2" hold="${3:-}" relay t0 rproxy_pid
    local marks=() exit_code cgroup phone_pids=()
    relay="$(relay_for_act "$act")"
    mark() {
        marks+=("{\"name\": \"$1\", \"ms\": $(($(now_ms) - t0))}")
        echo "[$(( ($(now_ms) - t0) / 1000 )) s] $1"
    }

    # systemd-run --scope runs rproxy in place, so $! is the pid of rproxy.
    # shellcheck disable=SC2046
    taskset -c "$SERVER_CPUS" systemd-run --user --scope --quiet \
        -p MemoryMax="$MEMORY_MAX" -p MemorySwapMax=0 \
        "$ROOT/rproxy" --port "$PROXY_PORT" \
        $(cert_files) --connections "$CONNECTIONS" \
        --client-timeout "$CLIENT_TIMEOUT" \
        --origin-timeout "$PROXY_ORIGIN_TIMEOUT" \
        --origin "127.0.0.1:$ORIGIN_PORT" --cache off \
        $(relay_flags "$relay") $(relay_stats_flags) $RPROXY_ARGS \
        >"$RUN/rproxy.log" 2>&1 &
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
    act_json_write "$act" "$relay" "$mode" "$rproxy_pid" "$cgroup" \
        "$([[ -n "$hold" ]] && echo true || echo false)"
    if [[ -n "$hold" ]]; then
        while [[ ! -f "$RUN/go" ]] && pid_is_alive "$rproxy_pid"; do
            sleep 0.1
        done
        # A new start time, so the memory chart starts with the Phones.
        act_json_write "$act" "$relay" "$mode" "$rproxy_pid" "$cgroup"
    fi
    t0="$(now_ms)"
    mark "start"

    phones_start phone_pids "$(arrival_for_act "$act")"
    printf '%s\n' "${phone_pids[@]}" >"$RUN/phones.pid"

    # Mark the first of: rproxy stops, all Phones end. Then the other.
    local ended pid alive
    while true; do
        set +e
        wait -n -p ended "$rproxy_pid" "${phone_pids[@]}"
        exit_code=$?
        set -e
        if [[ -n "$rproxy_pid" && "$ended" == "$rproxy_pid" ]]; then
            echo "$exit_code" >"$RUN/rproxy.exit"
            if [[ "$exit_code" == 137 ]]; then
                mark "rproxy killed"
                oom_check "$(basename "$cgroup")"
            else
                mark "rproxy exited $exit_code"
            fi
            rproxy_pid=""
        fi
        alive=()
        for pid in "${phone_pids[@]}"; do
            pid_is_alive "$pid" && alive+=("$pid")
        done
        phone_pids=("${alive[@]}")
        if [[ ${#phone_pids[@]} == 0 ]]; then
            mark "phones finished"
            break
        fi
        if [[ -z "$rproxy_pid" ]]; then
            wait "${phone_pids[@]}" || true
            mark "phones finished"
            break
        fi
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
    local act="$1" mode="${2:-dry}" hold="${3:-}" model="$ORIGIN_MODEL"
    [[ "$mode" == dry || "$mode" == rec ]] || die "mode is dry or rec"
    relay_for_act "$act" >/dev/null
    mix_tiers >/dev/null
    [[ "$act" == 5 ]] && model=pool
    if [[ -z "${RELAY_FLAGS+set}" ]]; then
        local help
        help="$("$ROOT/rproxy" --help 2>&1 || true)"
        [[ "$help" == *--relay* ]] || die "this rproxy has no --relay flag; rebuild it (make rproxy)"
    fi
    prepare
    act_stop
    origin_start "$model"
    rm -f "$RUN"/status*.json "$RUN/rproxy.exit" "$RUN/act.failed" \
        "$RUN/act.json" "$RUN/go" "$RUN/phones.log"
    rm -rf "$RUN/relay"
    mkdir -p "$RUN/relay"
    setsid bash "$DEMO/demo.sh" _run "$act" "$mode" "$hold" \
        >"$RUN/runner.log" 2>&1 </dev/null &
    echo $! >"$RUN/runner.pid"
    for _ in $(seq 100); do
        [[ -f "$RUN/act.failed" ]] && { cat "$RUN/runner.log"; die "Act $act failed"; }
        [[ -f "$RUN/act.json" ]] && break
        sleep 0.1
    done
    [[ -f "$RUN/act.json" ]] || die "rproxy did not answer in 10 s (see $RUN/runner.log)"
    echo "Act $act: rproxy --relay $(relay_for_act "$act"), Origin $model," \
        "$(mix_total) Phones in $(mix_tiers | wc -l) tiers ($mode)"
}

# One bystander curl, printed as a large plain banner.
present_bystander() {
    local line word
    line="$(RUN="$RUN" "$DEMO/bystander.sh" "$PROXY_PORT")"
    case "$line" in
        *"HTTP 200"*) word="HTTP 200" ;;
        *"connection refused"*) word="연결 거부" ;;
        *"no answer"*) word="응답 없음 (5 s)" ;;
        *) word="${line#bystander: }" ;;
    esac
    echo
    echo "    ##################################################"
    echo "    ##"
    echo "    ##     bystander:   $word"
    echo "    ##"
    echo "    ##################################################"
    echo "    ($line)"
    echo
}

present_wait() {
    if [[ -n "$PRESENT_PAUSE" ]]; then
        echo "    ... $PRESENT_PAUSE s"
        sleep "$PRESENT_PAUSE"
    else
        echo "    [Enter]"
        read -r _ || true
    fi
}

present_header() {
    echo
    echo "=== $* ==="
}

# The live flow of the talk. Each Phase waits for Enter.
present() {
    local pid i
    rm -f "$RUN/scores.json"     # the score table starts empty
    present_header "Phase 0  평소: Act 1 rproxy, Phone 없음"
    act_start 1 dry hold
    present_bystander
    present_wait

    present_header "Phase 1  unbounded: Phone 출발"
    touch "$RUN/go"
    pid="$(pid_read rproxy)"
    for i in $(seq $((PRESENT_DEATH_WAIT * 10))); do
        pid_is_alive "$pid" || break
        sleep 0.1
    done
    if pid_is_alive "$pid"; then
        echo "    rproxy is alive after $PRESENT_DEATH_WAIT s"
    else
        sleep 0.5    # let the runner record the exit
        # The same clock as the dashboard: from "started" of act.json.
        echo "    rproxy: $(cat "$RUN/rproxy.exit" 2>/dev/null || echo gone)" \
            "at $(python3 -c 'import json, os, sys
act = json.load(open(sys.argv[1] + "/act.json"))
exit_time = os.stat(sys.argv[1] + "/rproxy.exit").st_mtime
print(round(exit_time - act["started"], 1))' \
            "$RUN" 2>/dev/null || echo "?") s"
    fi
    present_bystander
    present_wait

    # Phase 2 is two wrong fixes in a row, with no Enter between them:
    # 2-A the 4 MiB whole-reply limit we had, 2-B a connection limit.
    present_header "Phase 2-A  reply-cap: the 4 MiB limit we had"
    act_start 2 dry
    sleep "$PRESENT_2A_BYSTANDER"
    present_bystander
    sleep $((PRESENT_2A_WAIT > PRESENT_2A_BYSTANDER ?
        PRESENT_2A_WAIT - PRESENT_2A_BYSTANDER : 0))
    present_header "Phase 2-B  conn-cap: a connection limit, set for memory"
    act_start 3 dry
    sleep "$PRESENT_2B_WAIT"
    present_bystander
    present_wait

    present_header "Phase 3  backpressure"
    act_start 4 dry
    sleep "$PRESENT_ACT4_WAIT"
    present_bystander
    present_wait

    act_stop
    origin_stop
    echo "stopped"
}

case "${1:-}" in
    act) [[ -n "${2:-}" ]] || die "usage: demo.sh act N [dry|rec]"
         act_start "$2" "${3:-dry}" ;;
    present) present ;;
    _run) act_run "$2" "$3" "${4:-}" ;;
    stop) act_stop; origin_stop; echo "stopped" ;;
    *) sed -n '2,18p' "$0"; exit 2 ;;
esac
