#!/usr/bin/env bash
# One curl of /index.html through rproxy, for the presenter to type in each
# Act (SPEC 12). It prints the status code and the time, or the curl error.
# It also writes $RUN/bystander.json for the dashboard, if that directory
# exists: first with "pending" when the curl starts, so the dashboard can
# count the wait, then with the result.
# Usage: bench/demo/bystander.sh [PORT]
#   BYSTANDER_WAIT (seconds, default 5) is the longest wait for the reply.
port="${1:-${PROXY_PORT:-19443}}"
wait="${BYSTANDER_WAIT:-5}"
run="${RUN:-$(dirname "$0")/run}"

# Writes bystander.json: $1 is the line, $2 ok (true or false), $3 pending.
bystander_publish() {
    [[ -d "$run" ]] || return 0
    printf '{"line": "%s", "ok": %s, "pending": %s, "start": %s, "time": %s}\n' \
        "$(tr -d '"\\' <<<"$1")" "$2" "$3" "$start" "$(date +%s.%N)" \
        >"$run/bystander.tmp"
    mv "$run/bystander.tmp" "$run/bystander.json"
}

start="$(date +%s.%N)"
bystander_publish "waiting" false true
out="$(curl -k -s -S -o /dev/null --max-time "$wait" \
    -w '%{http_code} %{time_total}' "https://127.0.0.1:$port/index.html" 2>&1)"
code=$?
case $code in
    0) line="$(awk '{ printf "bystander: HTTP %s in %.0f ms", $1, $2 * 1000 }' <<<"$out")" ;;
    7) line="bystander: connection refused" ;;
    28) line="bystander: no answer in $wait s" ;;
    *) line="bystander: failed, $(sed -n 's/^curl: ([0-9]*) //p' <<<"$out" | head -1)" ;;
esac
echo "$line"
ok=false
[[ "$line" == *"HTTP 200"* ]] && ok=true
bystander_publish "${line#bystander: }" "$ok" false
