#!/usr/bin/env bash
# Usage: bench/arena/spread.sh BASE_URL DURATION
# Crawler script: like named.sh, but it sends from 32 source addresses,
# 127.0.0.2 to 127.0.0.33 (curl --interface). Each batch uses the next
# address. A rate limit for each IP sees 32 slow clients, not one fast
# client. All addresses have the same JA4.
source "$(dirname "$0")/lib.sh"
PARALLEL=${PARALLEL:-8}
ADDRESS_FIRST=2
ADDRESS_COUNT=32
AGENT='Mozilla/5.0 AppleWebKit/537.36 (KHTML, like Gecko; compatible; GPTBot/1.1; +https://openai.com/gptbot)'

# spread_loop <loop index>
spread_loop() {
    local page=$(($1 * FILE_COUNT / PARALLEL)) address=$1 urls=()
    while :; do
        urls=()
        for _ in $(seq "$BATCH"); do
            urls+=("$(page_url "$page")")
            page=$((page + 1))
        done
        curl_batch -A "$AGENT" \
            --interface "127.0.0.$((ADDRESS_FIRST + address % ADDRESS_COUNT))" \
            -- "${urls[@]}"
        address=$((address + PARALLEL))
    done
}

for i in $(seq 0 $((PARALLEL - 1))); do
    loop_start spread_loop "$i"
done
arena_finish spread
