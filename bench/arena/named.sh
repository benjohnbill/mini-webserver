#!/usr/bin/env bash
# Usage: bench/arena/named.sh BASE_URL DURATION
# Crawler script: an honest crawler. Its User-Agent names it (GPTBot/1.1),
# and it walks all pages of the dataset as fast as it can.
source "$(dirname "$0")/lib.sh"
PARALLEL=${PARALLEL:-8}
# The User-Agent that OpenAI documents for GPTBot.
AGENT='Mozilla/5.0 AppleWebKit/537.36 (KHTML, like Gecko; compatible; GPTBot/1.1; +https://openai.com/gptbot)'

# named_loop <first page>: walks the pages in order from the first page.
named_loop() {
    local page=$1 urls=()
    while :; do
        urls=()
        for _ in $(seq "$BATCH"); do
            urls+=("$(page_url "$page")")
            page=$((page + 1))
        done
        curl_batch -A "$AGENT" -- "${urls[@]}"
    done
}

for i in $(seq 0 $((PARALLEL - 1))); do
    loop_start named_loop $((i * FILE_COUNT / PARALLEL))
done
arena_finish named
