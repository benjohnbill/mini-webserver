#!/usr/bin/env bash
# Usage: bench/arena/human.sh BASE_URL DURATION
# A slow Verification client. It loads the home page as a browser does
# (the page, then its CSS, image, script, and /api/time), then reads for
# 1 to 3 seconds, then loads it again. It is curl with the User-Agent of
# curl, so its JA4 is the JA4 of every Crawler script.
# It sends from 127.0.0.200, so a Scene can tell its lines in the rproxy
# log from the lines of a crawler.
source "$(dirname "$0")/lib.sh"
HUMAN_ADDRESS=127.0.0.200

human_loop() {
    while :; do
        curl_batch --interface "$HUMAN_ADDRESS" -- "$BASE/" \
            "$BASE/style.css" "$BASE/image.png" "$BASE/app.js" \
            "$BASE/api/time"
        sleep $((1 + RANDOM % 3))
    done
}

loop_start human_loop
arena_finish human
