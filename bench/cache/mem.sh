#!/usr/bin/env bash
# Usage: bench/cache/mem.sh PID...
# Prints the total Pss in KiB of each PID and all of its descendants.
# Pss, not RSS: after fork, a child shares the pages of its parent
# (copy-on-write) and the pages of a MAP_SHARED cache. RSS counts a shared
# page once in each process, so a sum of RSS counts it many times. Pss
# divides a shared page among the processes that map it, so the sum is the
# real memory of the process tree.
set -u

descendants() {
    local child
    for child in $(pgrep -P "$1"); do
        echo "$child"
        descendants "$child"
    done
}

total=0
for pid in "$@"; do
    for each in "$pid" $(descendants "$pid"); do
        pss=$(awk '/^Pss:/ {print $2}' "/proc/$each/smaps_rollup" 2>/dev/null)
        total=$((total + ${pss:-0}))
    done
done
echo "$total"
