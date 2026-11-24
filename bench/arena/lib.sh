# Shared parts of the Crawler scripts and human.sh. Each script runs as
#   bench/arena/<name>.sh BASE_URL DURATION
# and sources this file. It starts its loops with loop_start, then calls
# arena_finish. arena_finish waits DURATION seconds, stops the loops, and
# prints one line: "<name> sent=N mean_ms=M p95_ms=P <status>=<count> ...".
# A status of 000 is a request with no reply (a tarpit, a reset, or a
# timeout). A request that is in flight at the end is not counted.
set -u

BASE=${1:?usage: $0 BASE_URL DURATION}
DURATION=${2:?usage: $0 BASE_URL DURATION}
# The files of bench/cache/make-dataset.sh: www/bench/f000.txt to f199.txt.
FILE_COUNT=200
# One curl process sends BATCH requests, one after another. rproxy closes
# each connection after one reply, so each request is a new TLS
# connection. A batch saves the start of a curl process.
BATCH=${BATCH:-10}
# Each line of a loop file: "<status> <seconds>".
CURL=(curl -sk --max-time 30 -w '%{http_code} %{time_total}\n')
ARENA_WORK=$(mktemp -d)
LOOP_PIDS=()

# Job control puts each background loop in its own process group, so a
# stop also stops the curl process of the loop.
set -m

arena_cleanup() {
    for pid in "${LOOP_PIDS[@]}"; do
        kill -TERM -- "-$pid" 2>/dev/null
    done
    wait 2>/dev/null
    rm -rf "$ARENA_WORK"
}
trap arena_cleanup EXIT

# loop_start <function> [args...]: runs the function in the background.
# Its stdout goes to its own loop file.
loop_start() {
    "$@" >"$ARENA_WORK/loop.${#LOOP_PIDS[@]}" 2>/dev/null &
    LOOP_PIDS+=($!)
}

# curl_batch [curl options...] -- URL...: one curl process for all URLs.
# Each URL gets its own -o /dev/null, because -o and --out-null apply to
# one URL only.
curl_batch() {
    local options=() targets=() url
    while [ "$1" != -- ]; do
        options+=("$1")
        shift
    done
    shift
    for url in "$@"; do
        targets+=(-o /dev/null "$url")
    done
    "${CURL[@]}" "${options[@]}" "${targets[@]}"
}

# page_url <index>: the URL of dataset file index % FILE_COUNT.
page_url() {
    printf '%s/bench/f%03d.txt' "$BASE" $(($1 % FILE_COUNT))
}

arena_finish() {
    local name=$1
    sleep "$DURATION"
    for pid in "${LOOP_PIDS[@]}"; do
        kill -TERM -- "-$pid" 2>/dev/null
    done
    wait 2>/dev/null
    LOOP_PIDS=()
    cat "$ARENA_WORK"/loop.* 2>/dev/null | awk -v name="$name" '
        NF == 2 { sent++; time += $2; times[sent] = $2; count[$1]++ }
        END {
            # p95 by the nearest-rank method, as arena.py
            asort(times)
            rank = int(sent * 0.95)
            if (rank < sent * 0.95) rank++
            printf "%s sent=%d mean_ms=%.0f p95_ms=%.0f", name, sent,
                sent ? time * 1000 / sent : 0, sent ? times[rank] * 1000 : 0
            n = asorti(count, codes)
            for (i = 1; i <= n; i++) printf " %s=%d", codes[i], count[codes[i]]
            printf "\n"
        }'
}
