#!/usr/bin/env bash
# One curl of /index.html through rproxy, for the presenter to type in each
# Act (SPEC 12). It prints the status code and the time, or the curl error.
# Usage: bench/demo/bystander.sh [PORT]
port="${1:-${PROXY_PORT:-19443}}"
out="$(curl -k -s -S -o /dev/null --max-time 5 \
    -w '%{http_code} %{time_total}' "https://127.0.0.1:$port/index.html" 2>&1)"
code=$?
case $code in
    0) awk '{ printf "bystander: HTTP %s in %.0f ms\n", $1, $2 * 1000 }' <<<"$out" ;;
    7) echo "bystander: connection refused" ;;
    28) echo "bystander: no answer in 5 s" ;;
    *) echo "bystander: failed, $(sed -n 's/^curl: ([0-9]*) //p' <<<"$out" | head -1)" ;;
esac
