#!/usr/bin/env bash
# Usage: bench/arena/repeat.sh BASE_URL DURATION
# Crawler script: it downloads the same large file again and again, with
# no If-None-Match and no If-Modified-Since. Read the Docs reported this
# kind of crawler: it took the same files many times a day.
# The file is f019.txt (2 MiB). www/large.bin (10 MiB) is larger than
# ORIGIN_REPLY_MAX (4 MiB), so rproxy would only give 502 for it.
source "$(dirname "$0")/lib.sh"
PARALLEL=${PARALLEL:-4}

repeat_loop() {
    local url=$BASE/bench/f019.txt urls=()
    for _ in $(seq "$BATCH"); do
        urls+=("$url")
    done
    while :; do
        curl_batch -- "${urls[@]}"
    done
}

for _ in $(seq "$PARALLEL"); do
    loop_start repeat_loop
done
arena_finish repeat
