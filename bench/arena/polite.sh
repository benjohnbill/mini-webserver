#!/usr/bin/env bash
# Usage: bench/arena/polite.sh BASE_URL DURATION
# Crawler script: a crawler that obeys. It names itself, reads robots.txt
# once, and then walks the links of the home page and all pages of the
# dataset. It skips each target that a Disallow line of robots.txt
# starts (RFC 9309 section 2.2.2), so it never requests /trap/. It sends
# 1 request each second, one at a time.
source "$(dirname "$0")/lib.sh"
AGENT='Mozilla/5.0 (compatible; ArenaPoliteBot/1.0; +https://crawler.example/bot)'
# The time between two requests: a common crawl delay.
DELAY=1

# polite_get <target> [curl options...]: one request, then the delay.
polite_get() {
    local target=$1
    shift
    "${CURL[@]}" -A "$AGENT" "$@" "$BASE$target"
    sleep "$DELAY"
}

# The Disallow paths of robots.txt, one on each line. A short parse: it
# reads the Disallow lines of all groups, so it can skip more than a
# group for its User-Agent would. It never skips less.
robots_read() {
    polite_get /robots.txt -o "$ARENA_WORK/robots.txt"
    sed -n 's/^[Dd]isallow:[[:space:]]*\([^[:space:]#]*\).*/\1/p' \
        "$ARENA_WORK/robots.txt" | grep -v '^$' >"$ARENA_WORK/disallow"
}

is_allowed() {
    local path
    while read -r path; do
        [ "${1#"$path"}" != "$1" ] && return 1
    done <"$ARENA_WORK/disallow"
    return 0
}

# The targets of the href and src links of a page. A link with no "/" in
# front is relative to "/".
links_find() {
    grep -o -E '(href|src)="[^"]*"' "$1" | sed -E 's/^[a-z]+="//; s/"$//' |
        sed 's|^\([^/]\)|/\1|'
}

polite_loop() {
    local target page
    robots_read
    while :; do
        polite_get / -o "$ARENA_WORK/home.html"
        for target in $(links_find "$ARENA_WORK/home.html"); do
            is_allowed "$target" && polite_get "$target" -o /dev/null
        done
        for page in $(seq 0 $((FILE_COUNT - 1))); do
            target=$(printf '/bench/f%03d.txt' "$page")
            is_allowed "$target" && polite_get "$target" -o /dev/null
        done
    done
}

loop_start polite_loop
arena_finish polite
