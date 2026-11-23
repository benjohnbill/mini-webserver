#!/usr/bin/env bash
# Usage: bench/cache/make-dataset.sh
# Makes www/bench/f000.txt to f199.txt. Most files are 4 to 64 KiB. Five
# files are 2 MiB (f019, f059, f099, f139, f179), so the size admission has
# replies to reject. The sizes come from a fixed seed, so each run makes the
# same dataset.
set -u
cd "$(dirname "$0")/../.." || exit 1

DIR=www/bench
FILE_COUNT=200
mkdir -p "$DIR"
awk -v count="$FILE_COUNT" 'BEGIN {
    srand(7)
    for (i = 0; i < count; i++) {
        if (i % 40 == 19) {
            size = 2 * 1024 * 1024
        } else {
            size = int(4096 + rand() * (65536 - 4096))
        }
        printf "%03d %d\n", i, size
    }
}' | while read -r index size; do
    head -c "$size" /dev/zero | tr '\0' 'a' >"$DIR/f$index.txt"
done
du -sh "$DIR"
