#!/usr/bin/env bash
# Usage: bench/arena/scene.sh <crawler> [preset...]
# Crawlers: repeat longtail named disguised spread polite trapped
# revalidate solver signed.
# Presets: none cache rate-ip rate-ja4 all layers (default: the first
# five; layers needs the wiring of SPEC 11 steps 10 and 11).
# For each preset: starts an Origin (./server --model pool --delay
# DELAY_MS) and ./rproxy with the flags of the preset, runs the Crawler
# script and human.py side by side for DURATION seconds, stops both
# programs, and counts the lines of the rproxy log for each client kind.
# Output: docs/measurements/<date>-arena-<crawler>.md, one row for each
# preset and client kind. A Scene measures. It does not pass or fail.
set -u
cd "$(dirname "$0")/../.." || exit 1

CRAWLER=${1:?usage: bench/arena/scene.sh <crawler> [preset...]}
shift
PRESETS=("$@")
[ ${#PRESETS[@]} -gt 0 ] || PRESETS=(none cache rate-ip rate-ja4 all)
HOST=127.0.0.1
ORIGIN_PORT=${ORIGIN_PORT:-19281}
PX_PORT=${PX_PORT:-19280}
DELAY_MS=${DELAY_MS:-10}
DURATION=${DURATION:-10}
# human.py sends from this address (see human.py).
HUMAN_ADDRESS=127.0.0.200
DATE=$(date +%Y%m%d)
# CACHE_BYTES sets --cache-bytes of the cache presets. A cache smaller
# than the dataset (16.7 MiB) shows the long tail: rare pages miss.
CACHE_BYTES=${CACHE_BYTES:-}
CACHE_FLAG=${CACHE_BYTES:+ --cache-bytes $CACHE_BYTES}
OUT=docs/measurements/$DATE-arena-$CRAWLER${CACHE_BYTES:+-cache$CACHE_BYTES}.md
BASE=https://$HOST:$PX_PORT
WORK=$(mktemp -d)
ORIGIN_PID=
PX_PID=

# The layers preset: every defense of SPEC 11 "The layers". The rate key
# is the IP: human.py, solver.py, and signed.py share one JA4.
# robots.txt and the honeypot are files of www/, so they are always on.
# The proof of work protects /api/, where human.py also goes, with 16
# bits (the default difficulty 4 of Anubis: 4 hex zeros). Pay protects
# the dataset /bench/, which named.sh and signed.py walk.
LAYERS_FLAGS="--block-list bench/arena/layers-block.txt \
--ja4-list bench/arena/layers-ja4.txt --block-time 600 \
--rate 5 --burst 10 --pow-prefix /api/ --pow-bits 16 \
--pay-prefix /bench/ --bot-key tests/data/bot_test_keys.txt --cache on$CACHE_FLAG"

cleanup() {
    for pid in $PX_PID $ORIGIN_PID; do
        kill -TERM "$pid" 2>/dev/null && wait "$pid" 2>/dev/null
    done
    rm -rf "$WORK"
}
trap cleanup EXIT

# preset_flags <preset>: prints the rproxy flags of the preset.
preset_flags() {
    case $1 in
    none) echo "" ;;
    cache) echo "--cache on$CACHE_FLAG" ;;
    rate-ip) echo "--rate 5 --burst 10" ;;
    rate-ja4) echo "--rate 5 --burst 10 --rate-key ja4" ;;
    all) echo "--cache on$CACHE_FLAG --rate 5 --burst 10 --rate-key ja4" ;;
    layers) echo "$LAYERS_FLAGS" ;;
    layers-1loop) echo "$LAYERS_FLAGS --loops 1" ;;
    *) return 1 ;;
    esac
}

# script_find <name>: prints the path of the script: <name>.sh or
# <name>.py.
script_find() {
    local path
    for path in "bench/arena/$1.sh" "bench/arena/$1.py"; do
        [ -x "$path" ] && { echo "$path"; return 0; }
    done
    return 1
}

files_check() {
    case $CRAWLER in
    human | scene | lib | arena) CRAWLER_SCRIPT= ;;
    *) CRAWLER_SCRIPT=$(script_find "$CRAWLER") ;;
    esac
    [ -n "$CRAWLER_SCRIPT" ] || { echo "no Crawler script: $CRAWLER"; exit 1; }
    for preset in "${PRESETS[@]}"; do
        preset_flags "$preset" >/dev/null || { echo "no preset: $preset"; exit 1; }
    done
    for file in ./server ./rproxy build/cert.pem build/key.pem; do
        [ -e "$file" ] || { echo "$file is missing (make all cert)"; exit 1; }
    done
    [ -e www/bench/f199.txt ] || bench/cache/make-dataset.sh
    # human.py loads www/image.png. Without it, each page load gets a 404,
    # and the scanner rule (5 replies of 404) blocks human.py.
    [ -e www/image.png ] || make www >/dev/null
}

port_wait() {
    for _ in $(seq 50); do
        kill -0 "$1" 2>/dev/null || return 1
        nc -z "$HOST" "$2" 2>/dev/null && return 0
        sleep 0.1
    done
    return 1
}

programs_start() {
    ./server "$ORIGIN_PORT" www --model pool --delay "$DELAY_MS" \
        >/dev/null 2>"$WORK/origin.err" &
    ORIGIN_PID=$!
    port_wait "$ORIGIN_PID" "$ORIGIN_PORT" || { echo "Origin did not start"; exit 1; }
    # shellcheck disable=SC2086 # the flags are words
    ./rproxy --port "$PX_PORT" --cert build/cert.pem --key build/key.pem \
        --origin "$HOST:$ORIGIN_PORT" $1 >/dev/null 2>"$WORK/rproxy.log" &
    PX_PID=$!
    port_wait "$PX_PID" "$PX_PORT" || { echo "rproxy did not start"; exit 1; }
}

programs_stop() {
    for pid in $PX_PID $ORIGIN_PID; do
        kill -TERM "$pid" 2>/dev/null && wait "$pid" 2>/dev/null
    done
    PX_PID=
    ORIGIN_PID=
}

# client_view <summary line> <field>: a field of the line that a Crawler
# script or human.py prints: sent, mean_ms, pow_cpu_s, or the status
# counts. A field that the line does not have is "-". Only the Python
# scripts print pow_cpu_s.
client_view() {
    local value
    case $2 in
    statuses) value=$(echo "$1" | tr ' ' '\n' | grep -E '^[0-9]{3}=' | paste -sd' ') ;;
    *) value=$(echo "$1" | tr ' ' '\n' | sed -n "s/^$2=//p") ;;
    esac
    echo "${value:--}"
}

# log_count <kind> <client IP test>: prints the counts of the rproxy log
# lines of one client kind: lines pass deny slow tarpit drop challenge pay
# hits origin MiB.
# A log line: client IP, JA4, Verdict, cache, Origin index, status, reply
# bytes. A reply from the Origin is a line with an Origin index, no cache
# hit, and no 502 or 504 of rproxy.
# A line that starts with "behavior" holds the behavior counters of a
# client IP. It is not a connection, so it is skipped.
log_count() {
    awk -v human="$HUMAN_ADDRESS" -v want="$1" '
        NF != 7 || $1 == "cache" || $1 == "behavior" { next }
        { kind = $1 == human ? "human" : "crawler" }
        kind != want { next }
        { lines++; verdict[$3]++ }
        $4 == "hit" { hits++ }
        $5 != "-" { origin++ }
        $5 != "-" && $4 != "hit" && $6 != "502" && $6 != "504" { bytes += $7 }
        END {
            printf "%d | %d | %d | %d | %d | %d | %d | %d | %d | %d | %.1f\n",
                lines, verdict["pass"], verdict["deny"], verdict["slow"],
                verdict["tarpit"], verdict["drop"], verdict["challenge"],
                verdict["pay"], hits, origin, bytes / 1048576
        }' "$WORK/rproxy.log"
}

# preset_run <preset>: runs one preset and appends its two rows.
preset_run() {
    local preset=$1 flags
    flags=$(preset_flags "$preset")
    echo "== $CRAWLER, preset $preset: ${flags:-no flags}"
    programs_start "$flags"
    "$CRAWLER_SCRIPT" "$BASE" "$DURATION" >"$WORK/crawler.out" &
    local crawler_pid=$!
    bench/arena/human.py "$BASE" "$DURATION" >"$WORK/human.out" &
    local human_pid=$!
    wait "$crawler_pid" "$human_pid"
    programs_stop
    local kind summary
    for kind in crawler human; do
        summary=$(cat "$WORK/$kind.out")
        echo "   $summary"
        printf '| %s | %s | %s | %s | %s | %s | %s |\n' "$preset" \
            "$([ "$kind" = human ] && echo human || echo "$CRAWLER")" \
            "$(client_view "$summary" sent)" \
            "$(client_view "$summary" statuses)" \
            "$(client_view "$summary" mean_ms)" \
            "$(client_view "$summary" pow_cpu_s)" \
            "$(log_count "$kind")" >>"$WORK/rows"
    done
}

doc_write() {
    cat >"$OUT" <<EOF
# Arena Scene: $CRAWLER ($DATE)

Command: \`bench/arena/scene.sh $CRAWLER ${PRESETS[*]}\`. Commit: \`$(git rev-parse --short HEAD)\`. Duration: $DURATION s for each preset. Origin: \`./server --model pool --delay $DELAY_MS\`. \`bench/arena/human.py\` runs beside the Crawler script in each preset.

A Scene measures. It does not pass or fail. The owner slots \`ja4_format\`, \`rate_limit_take\`, and \`verdict_decide\` decide what a preset can do (SPEC §11 "Arena").

Presets:

| Preset | rproxy flags |
|---|---|
EOF
    for preset in "${PRESETS[@]}"; do
        printf '| %s | `%s` |\n' "$preset" "$(preset_flags "$preset")" >>"$OUT"
    done
    cat >>"$OUT" <<'EOF'

Columns:

- Sent, Statuses, and Mean ms are the view of the client: the requests that ended before the stop, their status codes (000: no reply), and their mean time. PoW CPU s is the CPU time that the client spent on proof of work solutions (Python clients only).
- The other columns count the rproxy log lines of the client. Pass, Deny, Slow, Tarpit, Drop, Challenge, and Pay are the Verdicts. Drop is the connection Verdict: no TLS byte goes out. To Origin counts the requests that rproxy sent to the Origin. MiB from Origin is the sum of the reply bytes of those requests.

| Preset | Client | Sent | Statuses | Mean ms | PoW CPU s | Log lines | Pass | Deny | Slow | Tarpit | Drop | Challenge | Pay | Cache hits | To Origin | MiB from Origin |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
EOF
    cat "$WORK/rows" >>"$OUT"
}

files_check
: >"$WORK/rows"
for preset in "${PRESETS[@]}"; do
    preset_run "$preset"
done
doc_write
echo "wrote $OUT"
