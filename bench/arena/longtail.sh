#!/usr/bin/env bash
# Usage: bench/arena/longtail.sh BASE_URL DURATION
# Crawler script: it reads pages of the dataset in a uniform random order,
# so rare pages get as many requests as popular pages. A human mostly reads
# a few popular pages. Needs bench/cache/make-dataset.sh.
source "$(dirname "$0")/lib.sh"
PARALLEL=${PARALLEL:-8}

longtail_loop() {
    local urls=()
    while :; do
        urls=()
        for _ in $(seq "$BATCH"); do
            urls+=("$(page_url $((RANDOM % FILE_COUNT)))")
        done
        curl_batch -- "${urls[@]}"
    done
}

for _ in $(seq "$PARALLEL"); do
    loop_start longtail_loop
done
arena_finish longtail
