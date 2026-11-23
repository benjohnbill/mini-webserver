#!/usr/bin/env bash
# Usage: bench/cache/run.sh [phase...]   (default: P0 P1 P2 P3)
# Phases: P0 P1 P2 P3 (P3 = P3-evict P3-ttl P3-lock).
# Each phase changes ONE cache axis. The other axes stay at their defaults.
# Output: docs/measurements/<date>-rproxy-cache-<phase>.md, and the raw logs
# in docs/measurements/<date>-rproxy-cache-raw/.
#
# Each run: an Origin (./server --model fork --delay DELAY_MS, so the
# delay does not serialize the Origin) and ./rproxy in front of it, over
# TLS. rproxy has only the pool model.
# wrk warms up for WARMUP s, then measures for DURATION s with zipf.lua.
# mem.sh samples the Pss of rproxy 2 s before the end. SIGTERM stops
# rproxy, and the script reads the "cache ..." line of its stderr.
set -u
cd "$(dirname "$0")/../.." || exit 1

SERVER=./server
HOST=127.0.0.1
ORIGIN_PORT=${ORIGIN_PORT:-19181}
PX_PORT=${PX_PORT:-19180}
DELAY_MS=${DELAY_MS:-10}
WARMUP=${WARMUP:-3}
DURATION=${DURATION:-10}
CONNECTIONS=${CONNECTIONS:-32}
# 8: with rproxy, the 8 threads are full, and more threads give less
# req/s (docs/measurements/20261124-rproxy-cache-parity.md).
THREADS=${THREADS:-8}
TIGHT_BYTES=${TIGHT_BYTES:-4194304}
LUA=bench/cache/zipf.lua
DATE=$(date +%Y%m%d)
OUT=docs/measurements
URL=https://localhost:$PX_PORT
RAW=$OUT/$DATE-rproxy-cache-raw
WORK=$(mktemp -d)
ORIGIN_PID=
PX_PID=

cleanup() {
    for pid in $PX_PID $ORIGIN_PID; do
        kill -TERM "$pid" 2>/dev/null && wait "$pid" 2>/dev/null
    done
    rm -rf "$WORK"
}
trap cleanup EXIT

tool_check() {
    if ! command -v wrk >/dev/null; then
        echo "wrk is not installed. This script needs wrk for the Lua path"
        echo "script and the X-Cache count; hey and ab cannot run zipf.lua."
        exit 1
    fi
    echo "load tool: wrk ($(command -v wrk))"
}

port_wait() {
    for _ in $(seq 50); do
        kill -0 "$1" 2>/dev/null || return 1
        nc -z "$HOST" "$2" 2>/dev/null && return 0
        sleep 0.1
    done
    return 1
}

origin_start() {
    "$SERVER" "$ORIGIN_PORT" www --model fork --delay "$DELAY_MS" \
        >/dev/null 2>"$WORK/origin.err" &
    ORIGIN_PID=$!
    port_wait "$ORIGIN_PID" "$ORIGIN_PORT" || { echo "Origin did not start"; exit 1; }
}

# Prints: req/s p99_ms errors client_hits client_misses
wrk_parse() {
    awk '
        /Requests\/sec/ { rps = $2 }
        /^ +99%/ {
            value = $2
            if (value ~ /us$/) { sub(/us$/, "", value); p99 = value / 1000 }
            else if (value ~ /ms$/) { sub(/ms$/, "", value); p99 = value }
            else if (value ~ /s$/) { sub(/s$/, "", value); p99 = value * 1000 }
        }
        /Non-2xx/ { errors += $NF }
        /Socket errors/ {
            gsub(/,/, "")
            errors += $4 + $6 + $8 + $10
        }
        /^xcache/ { hits = $3; misses = $5 }
        END { printf "%s %s %d %d %d\n", rps, p99, errors, hits, misses }
    ' "$1"
}

# Prints the fields of the "cache ..." line: hits misses rejects evictions
# expired bytes.
counters_sum() {
    awk '/^cache / {
        for (i = 3; i <= NF; i++) { split($i, pair, "="); sum[pair[1]] += pair[2] }
        seen = 1
    }
    END {
        if (!seen) { print "-"; exit }
        printf "h=%d m=%d r=%d e=%d x=%d b=%.1fMiB\n", sum["hits"],
            sum["misses"], sum["rejects"], sum["evictions"], sum["expired"],
            sum["bytes"] / 1048576
    }' "$1"
}

# proxy_start <stderr file> <rproxy args...>
proxy_start() {
    local err=$1
    shift
    ./rproxy --port "$PX_PORT" --cert build/cert.pem --key build/key.pem \
        --origin "$HOST:$ORIGIN_PORT" "$@" >/dev/null 2>"$err" &
    PX_PID=$!
}

# rproxy writes one log line for each exchange to stderr. The raw log keeps
# the "cache ..." line and the count of each "verdict cache status" triple.
log_keep() {
    awk '/^cache / { print; next }
        NF == 7 { count[$3 " " $4 " " $6]++ }
        END { for (key in count) print "exchanges", key, count[key] }' \
        "$1" | sort >"$2"
}

# run_variant <table> <variant> <runs> <rproxy args...>
# Appends one row to <table>: the mean of the runs, and the min to max of
# req/s when runs > 1.
run_variant() {
    local table=$1 variant=$2 runs=$3
    shift 3
    local rps_list=() p99_sum=0 err_sum=0 hit_sum=0 miss_sum=0 pss_sum=0
    local counters=-
    for run in $(seq "$runs"); do
        local tag="$variant-pool-$run"
        tag=${tag//[^A-Za-z0-9_.-]/_}
        proxy_start "$WORK/proxy.err" "$@"
        port_wait "$PX_PID" "$PX_PORT" || { echo "proxy did not start: $*"; exit 1; }
        wrk -t"$THREADS" -c"$CONNECTIONS" -d"${WARMUP}s" -s "$LUA" \
            "$URL" >/dev/null 2>&1
        wrk -t"$THREADS" -c"$CONNECTIONS" -d"${DURATION}s" --latency -s "$LUA" \
            "$URL" >"$RAW/$tag.wrk.txt" 2>&1 &
        local wrk_pid=$!
        sleep $((DURATION - 2))
        local pss_kib
        pss_kib=$(bench/cache/mem.sh "$PX_PID")
        wait "$wrk_pid"
        kill -TERM "$PX_PID"
        wait "$PX_PID" 2>/dev/null
        PX_PID=
        log_keep "$WORK/proxy.err" "$RAW/$tag.server.err"
        local rps p99 errors hits misses
        read -r rps p99 errors hits misses < <(wrk_parse "$RAW/$tag.wrk.txt")
        counters=$(counters_sum "$RAW/$tag.server.err")
        echo "  $variant run $run: $rps req/s, p99 $p99 ms," \
            "errors $errors, X-Cache $hits/$misses, Pss $((pss_kib / 1024)) MiB," \
            "rproxy $counters"
        rps_list+=("$rps")
        p99_sum=$(awk -v a="$p99_sum" -v b="$p99" 'BEGIN { print a + b }')
        err_sum=$((err_sum + errors))
        hit_sum=$((hit_sum + hits))
        miss_sum=$((miss_sum + misses))
        pss_sum=$((pss_sum + pss_kib))
    done
    awk -v variant="$variant" -v runs="$runs" \
        -v rps_all="${rps_list[*]}" -v p99_sum="$p99_sum" -v errors="$err_sum" \
        -v hits="$hit_sum" -v misses="$miss_sum" -v pss="$pss_sum" \
        -v counters="$counters" 'BEGIN {
        n = split(rps_all, list, " ")
        min = list[1]; max = list[1]; sum = 0
        for (i = 1; i <= n; i++) {
            sum += list[i]
            if (list[i] < min) min = list[i]
            if (list[i] > max) max = list[i]
        }
        rps = sprintf("%.0f", sum / n)
        if (n > 1) rps = rps sprintf(" (%.0f to %.0f)", min, max)
        rate = hits + misses > 0 ? sprintf("%.1f%%", 100 * hits / (hits + misses)) : "-"
        printf "| %s | pool | %s | %.2f | %s | %.1f | %d | %s | %s |\n",
            variant, rps, p99_sum / n, rate, pss / n / 1024, errors,
            counters, runs
    }' >>"$table"
}

table_start() {
    local phase=$1 title=$2
    local table=$OUT/$DATE-rproxy-cache-$phase.md
    {
        echo "# $phase: $title"
        echo
        echo "Made by \`bench/cache/run.sh $phase\` at $(date '+%Y-%m-%d %H:%M')," \
            "commit $(git rev-parse --short HEAD)."
        echo "Origin: \`--model fork --delay $DELAY_MS\`. Load:" \
            "\`wrk -t$THREADS -c$CONNECTIONS -d${DURATION}s --latency -s $LUA $URL\`" \
            "after a ${WARMUP} s warm-up. Dataset: \`www/bench\`," \
            "200 files, Zipf s = 1."
        echo
        echo "- hit rate: X-Cache HIT / (HIT + MISS) seen by wrk, in the measured window."
        echo "- Pss: rproxy, sampled 2 s before the end."
        echo "- errors: non-2xx replies plus socket errors. A row is valid only with 0."
        echo "- rproxy counters: hits, misses, rejects, evictions, expired;" \
            "warm-up included; b = bytes in the cache at exit;" \
            "the last run when runs > 1."
        echo "- p99 and Pss are the mean of the runs. req/s shows the range when runs > 1."
        echo
        echo "| variant | model | req/s | p99 ms | hit rate | Pss MiB | errors | rproxy counters | runs |"
        echo "|---|---|---|---|---|---|---|---|---|"
    } >"$table"
    echo "== $phase: $title -> $table" >&2
    echo "$table"
}

phase_p0() {
    local table
    table=$(table_start P0 "baseline, cache off, twice (noise floor)")
    run_variant "$table" off 2 --cache off
}

phase_p1() {
    local table
    table=$(table_start P1 "cache off and on")
    run_variant "$table" off 1 --cache off
    run_variant "$table" on 1 --cache on
}

phase_p2() {
    local table
    table=$(table_start P2 "admission, --cache-bytes $TIGHT_BYTES")
    for admit in all size second; do
        run_variant "$table" "admit $admit" 1 --cache on \
            --cache-bytes "$TIGHT_BYTES" --cache-admit "$admit"
    done
}

phase_p3_evict() {
    local table
    table=$(table_start P3-evict "eviction, --cache-bytes $TIGHT_BYTES (working set 17 MiB)")
    for evict in fifo lru; do
        run_variant "$table" "evict $evict" 1 --cache on \
            --cache-bytes "$TIGHT_BYTES" --cache-evict "$evict"
    done
}

phase_p3_ttl() {
    local table
    table=$(table_start P3-ttl "TTL 1 s and 60 s")
    for ttl in 1 60; do
        run_variant "$table" "ttl $ttl" 1 --cache on --cache-ttl "$ttl"
    done
}

phase_p3_lock() {
    local table
    table=$(table_start P3-lock "lock global and striped, pool with 4 and 16 Workers")
    for workers in 4 16; do
        for lock in global striped; do
            run_variant "$table" "lock $lock, workers $workers" \
                "${LOCK_RUNS:-3}" \
                --cache on --cache-lock "$lock" --workers "$workers"
        done
    done
}

tool_check
make -s server rproxy cert || exit 1
[ -f www/bench/f199.txt ] || bench/cache/make-dataset.sh
mkdir -p "$RAW"
origin_start

PHASES=("$@")
[ ${#PHASES[@]} -eq 0 ] && PHASES=(P0 P1 P2 P3)
for phase in "${PHASES[@]}"; do
    case "$phase" in
    P0) phase_p0 ;;
    P1) phase_p1 ;;
    P2) phase_p2 ;;
    P3) phase_p3_evict; phase_p3_ttl; phase_p3_lock ;;
    P3-evict) phase_p3_evict ;;
    P3-ttl) phase_p3_ttl ;;
    P3-lock) phase_p3_lock ;;
    *) echo "unknown phase: $phase"; exit 2 ;;
    esac
done
