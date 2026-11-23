#!/usr/bin/env bash
# Usage: bench/arena/revalidate.sh BASE_URL DURATION
# Crawler script: repeat.sh with If-None-Match. Each loop downloads
# f019.txt (2 MiB) once and keeps the ETag of the reply. Then it asks for
# the same file again and again with If-None-Match. With the cache on,
# rproxy answers 304 with no body. A reply with no ETag leaves the
# requests unconditional, as repeat.sh.
source "$(dirname "$0")/lib.sh"
PARALLEL=${PARALLEL:-4}

# revalidate_loop <loop index>
revalidate_loop() {
    local url=$BASE/bench/f019.txt headers=$ARENA_WORK/headers.$1 etag
    local urls=() condition=()
    "${CURL[@]}" -D "$headers" -o /dev/null "$url"
    etag=$(sed -n 's/^[Ee][Tt][Aa][Gg]:[[:space:]]*//p' "$headers" | tr -d '\r')
    [ -n "$etag" ] && condition=(-H "If-None-Match: $etag")
    for _ in $(seq "$BATCH"); do
        urls+=("$url")
    done
    while :; do
        curl_batch "${condition[@]}" -- "${urls[@]}"
    done
}

for i in $(seq 0 $((PARALLEL - 1))); do
    loop_start revalidate_loop "$i"
done
arena_finish revalidate
