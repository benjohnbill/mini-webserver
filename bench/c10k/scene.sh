#!/usr/bin/env bash
# Usage: bench/c10k/scene.sh
# The c10k Scene: COUNT TLS connections to rproxy are open at the same time,
# then each one gets a small file (SIZE bytes). It measures the memory of
# rproxy for each open connection, the replies, and the latency of the
# burst. A Scene measures. It does not pass or fail.
#
# The file is small on purpose: the reply fits in the kernel socket buffers,
# so rproxy holds no reply bytes. What is left is the cost of a connection.
# Output: docs/measurements/<date>-c10k-<count>.md
set -u
cd "$(dirname "$0")/../.." || exit 1

COUNT="${COUNT:-10000}"
SIZE="${SIZE:-10240}"
PROCS="${PROCS:-8}"     # client processes
RAMP="${RAMP:-3}"       # seconds to open all connections
HOLD="${HOLD:-0.5}"     # seconds of wait after the ramp; RAMP + HOLD < 5
ORIGIN_PORT="${ORIGIN_PORT:-19180}"
PROXY_PORT="${PROXY_PORT:-19543}"
FILE=www/bench/c10k.bin

# rproxy and the Origin get the lower half of the CPUs, the clients the
# upper half, as in bench/demo/demo.sh.
CPU_COUNT="$(nproc)"
CPU_HALF=$((CPU_COUNT / 2 > 0 ? CPU_COUNT / 2 : 1))
SERVER_CPUS="${SERVER_CPUS:-0-$((CPU_HALF - 1))}"
CLIENT_CPUS="${CLIENT_CPUS:-$((CPU_COUNT > 1 ? CPU_HALF : 0))-$((CPU_COUNT - 1))}"
LOOPS="${LOOPS:-$CPU_HALF}"

WORK=$(mktemp -d)
ORIGIN_PID=
RPROXY_PID=

cleanup() {
    for pid in "$RPROXY_PID" "$ORIGIN_PID"; do
        if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
            kill -TERM "$pid"
            wait "$pid" 2>/dev/null
        fi
    done
    mkdir -p bench/c10k/run && cp "$WORK"/*.log bench/c10k/run/ 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT

die() { echo "c10k: $*" >&2; exit 1; }

# One rproxy connection is a client socket and an Origin socket.
nofile_check() {
    local need=$((COUNT * 2 + 1000))
    [ "$(ulimit -n)" -ge "$need" ] ||
        die "ulimit -n is $(ulimit -n); the Scene needs $need"
}

file_make() {
    mkdir -p "$(dirname "$FILE")"
    if [ ! -f "$FILE" ] || [ "$(stat -c %s "$FILE")" != "$SIZE" ]; then
        head -c "$SIZE" /dev/urandom >"$FILE"
    fi
}

port_wait() {
    for _ in $(seq 50); do
        kill -0 "$1" 2>/dev/null || return 1
        nc -z 127.0.0.1 "$2" 2>/dev/null && return 0
        sleep 0.1
    done

    return 1
}

servers_start() {
    taskset -c "$SERVER_CPUS" ./server "$ORIGIN_PORT" www --model epoll \
        >"$WORK/origin.log" 2>&1 &
    ORIGIN_PID=$!
    port_wait "$ORIGIN_PID" "$ORIGIN_PORT" ||
        { cat "$WORK/origin.log"; die "the Origin did not start"; }
    taskset -c "$SERVER_CPUS" ./rproxy --port "$PROXY_PORT" \
        --cert build/cert.pem --key build/key.pem \
        --origin "127.0.0.1:$ORIGIN_PORT" --cache off \
        --loops "$LOOPS" --connections $((COUNT + 1000)) \
        >"$WORK/rproxy.log" 2>&1 &
    RPROXY_PID=$!
    port_wait "$RPROXY_PID" "$PROXY_PORT" ||
        { cat "$WORK/rproxy.log"; die "rproxy did not start"; }
}

# Prints the result section of the report from the JSON of clients.py.
report_body() {
    python3 - "$1" <<'EOF'
import json, sys
r = json.load(open(sys.argv[1]))
mib = lambda n: f"{n / 2**20:.1f} MiB"
per = lambda a, b: (f"{(r[a] - r[b]) / max(1, r['connected']) / 1024:.1f} KiB"
                    if r["connected"] else "-")
ms = lambda v: "-" if v is None else f"{v:.0f} ms"
rows = [
    ("connections asked", r["count"]),
    ("connections open at the go time", f"{r['connected']} ({r['late']} late)"),
    ("replies by status", ", ".join(f"{k}: {v}" for k, v in sorted(r["statuses"].items())) or "-"),
    (f"full bodies ({r['path']})", r["full_body"]),
    ("errors (stage:kind)", ", ".join(f"{k}: {v}" for k, v in sorted(r["errors"].items())) or "none"),
    ("rproxy RSS idle, held, peak", f"{mib(r['rss_idle'])}, {mib(r['rss_held'])}, {mib(r['rss_peak'])}"),
    ("rproxy RSS for each open connection", per("rss_held", "rss_idle")),
    ("kernel TCP memory idle, held", f"{mib(r['tcp_mem_idle'])}, {mib(r['tcp_mem_held'])}"),
    ("kernel TCP memory for each open connection", per("tcp_mem_held", "tcp_mem_idle")),
    ("rproxy fds held, peak", f"{r['fds_held']}, {r['fds_peak']}"),
    ("TLS connect p50, p99, max", f"{ms(r['connect_ms']['p50'])}, {ms(r['connect_ms']['p99'])}, {ms(r['connect_ms']['max'])}"),
    ("reply latency p50, p95, p99, max", f"{ms(r['latency_ms']['p50'])}, {ms(r['latency_ms']['p95'])}, {ms(r['latency_ms']['p99'])}, {ms(r['latency_ms']['max'])}"),
]
print("| Measure | Value |\n|---|---|")
for name, value in rows:
    print(f"| {name} | {value} |")
EOF
}

nofile_check
make -s server rproxy cert || die "build failed"
file_make
servers_start
COMMIT=$(git rev-parse --short HEAD 2>/dev/null || echo none)
echo "c10k: $COUNT connections, $SIZE bytes, commit $COMMIT"
taskset -c "$CLIENT_CPUS" python3 bench/c10k/clients.py --pid "$RPROXY_PID" \
    --port "$PROXY_PORT" --count "$COUNT" --procs "$PROCS" \
    --ramp "$RAMP" --hold "$HOLD" --size "$SIZE" >"$WORK/result.json" ||
    die "clients.py failed"
kill -0 "$RPROXY_PID" 2>/dev/null || echo "c10k: rproxy died during the run"

OUT="docs/measurements/$(date +%Y%m%d-%H%M)-c10k-$COUNT.md"
{
    echo "# c10k Scene, $COUNT connections"
    echo
    echo "- Date: $(date '+%Y-%m-%d %H:%M'). Commit: \`$COMMIT\`."
    echo "- Command: \`COUNT=$COUNT SIZE=$SIZE PROCS=$PROCS RAMP=$RAMP HOLD=$HOLD bench/c10k/scene.sh\`."
    echo "- rproxy: \`--loops $LOOPS --connections $((COUNT + 1000)) --cache off\`, CPUs $SERVER_CPUS. Origin: \`./server --model epoll\`, CPUs $SERVER_CPUS. Clients: CPUs $CLIENT_CPUS."
    echo "- Idle is before the first connection. Held is at the go time: all connections are open, after the TLS handshake, and no request is sent yet."
    echo
    report_body "$WORK/result.json"
    echo
    echo '```json'
    cat "$WORK/result.json"
    echo '```'
} >"$OUT"
cat "$OUT"
