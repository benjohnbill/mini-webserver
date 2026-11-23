#!/usr/bin/env bash
# Usage: bench/arena/trapped.sh BASE_URL DURATION
# Crawler script: a crawler that does not read robots.txt. It loads the
# home page, follows every link on it, the hidden /trap/ link too, and
# then walks the next BATCH pages of the dataset. Then it starts again.
# Its User-Agent is the default of curl. The honeypot puts its IP in the
# block table, so its next connections drop at the connection stage.
source "$(dirname "$0")/lib.sh"
PARALLEL=${PARALLEL:-4}

# The targets of the href and src links of a page. A link with no "/" in
# front is relative to "/".
links_find() {
    grep -o -E '(href|src)="[^"]*"' "$1" | sed -E 's/^[a-z]+="//; s/"$//' |
        sed 's|^\([^/]\)|/\1|'
}

# trapped_loop <first page>
trapped_loop() {
    local page=$1 home=$ARENA_WORK/home.$1.html urls=() target
    while :; do
        : >"$home"
        "${CURL[@]}" -o "$home" "$BASE/"
        urls=()
        for target in $(links_find "$home"); do
            urls+=("$BASE$target")
        done
        for _ in $(seq "$BATCH"); do
            urls+=("$(page_url "$page")")
            page=$((page + 1))
        done
        curl_batch -- "${urls[@]}"
    done
}

for i in $(seq 0 $((PARALLEL - 1))); do
    loop_start trapped_loop $((i * FILE_COUNT / PARALLEL))
done
arena_finish trapped
