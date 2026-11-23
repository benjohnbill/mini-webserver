#!/usr/bin/env bash
# Usage: bench/arena/disguised.sh BASE_URL DURATION
# Crawler script: like named.sh, but the User-Agent says Chrome on macOS.
# The TLS is still the TLS of curl, so its JA4 is the JA4 of curl, not of
# Chrome. Only a check of the JA4 against the User-Agent can see this.
source "$(dirname "$0")/lib.sh"
PARALLEL=${PARALLEL:-8}
AGENT='Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36'

disguised_loop() {
    local page=$1 urls=()
    while :; do
        urls=()
        for _ in $(seq "$BATCH"); do
            urls+=("$(page_url "$page")")
            page=$((page + 1))
        done
        curl_batch -A "$AGENT" -H 'Accept: text/html,*/*;q=0.8' \
            -H 'Accept-Language: en-US,en;q=0.9' -- "${urls[@]}"
    done
}

for i in $(seq 0 $((PARALLEL - 1))); do
    loop_start disguised_loop $((i * FILE_COUNT / PARALLEL))
done
arena_finish disguised
