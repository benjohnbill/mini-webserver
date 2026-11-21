#!/usr/bin/env bash
# Usage: bench/all.sh [scene...]   (default: S2 S2b S3)
# Runs each Scene on all four models. S2 and S2b measure waiting time, so
# the four models run in parallel, one port each. S3 measures throughput,
# so the models run one after another and do not share the CPU.
# Output: docs/measurements/<time>/<scene>.txt, then a summary.
set -u
cd "$(dirname "$0")/.." || exit 1

MODELS=(iter fork pool epoll)
declare -A PORTS=([iter]=18081 [fork]=18082 [pool]=18083 [epoll]=18084)
SCENES=("$@")
[ ${#SCENES[@]} -eq 0 ] && SCENES=(S2 S2b S3)

make -s server || exit 1
OUT=docs/measurements/$(date +%Y%m%d-%H%M)
mkdir -p "$OUT"

# Prints the highest count of server processes on a port, until stopped.
count_processes() {
    local max=0 count
    while sleep 0.2; do
        count=$(pgrep -fc "server $1 www") || count=0
        [ "$count" -gt "$max" ] && max=$count && echo "$max" >"$2"
    done
}

run_parallel() {
    local scene=$1 pids=()
    for model in "${MODELS[@]}"; do
        PORT=${PORTS[$model]} bench/scene.sh "$scene" "$model" \
            >"$OUT/$scene-$model.part" 2>&1 &
        pids+=($!)
    done
    wait "${pids[@]}"
}

run_serial() {
    local scene=$1
    for model in "${MODELS[@]}"; do
        local port=${PORTS[$model]} counter=
        if [ "$model" = fork ]; then
            count_processes "$port" "$OUT/fork-max-processes" &
            counter=$!
        fi
        PORT=$port bench/scene.sh "$scene" "$model" >"$OUT/$scene-$model.part" 2>&1
        if [ -n "$counter" ]; then
            kill "$counter" 2>/dev/null
            wait "$counter" 2>/dev/null
            echo "max server processes: $(cat "$OUT/fork-max-processes" 2>/dev/null || echo ?)" \
                >>"$OUT/$scene-$model.part"
        fi
    done
}

for scene in "${SCENES[@]}"; do
    echo "== $scene: $(date +%H:%M:%S)"
    case "$scene" in
    S3) run_serial "$scene" ;;
    *) run_parallel "$scene" ;;
    esac
    for model in "${MODELS[@]}"; do
        cat "$OUT/$scene-$model.part"
        echo
    done >"$OUT/$scene.txt"
    rm -f "$OUT/$scene"-*.part
done

echo
echo "== summary ($OUT)"
for scene in "${SCENES[@]}"; do
    echo "-- $scene"
    awk '/^scene /{model=$4; sub(/,$/, "", model)}
         /^curl time_total/{getline; printf "  %-6s %s\n", model, $0}
         /^wrk /{conn=$3}
         /Requests\/sec/{printf "  %-6s %-6s %s req/s\n", model, conn, $2}
         /Socket errors/{printf "  %-6s %-6s %s\n", model, conn, $0}
         /^max server processes/{printf "  %-6s %s\n", model, $0}
         /^server: /{printf "  %-6s %s\n", model, $0}' "$OUT/$scene.txt"
done
