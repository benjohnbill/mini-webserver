#!/usr/bin/env bash
# Opens the lb.sh demo as one Herdr tab split into three panes:
#   backend (left top)  : lb.sh up, then follows the front/b1..b3 access logs
#   client  (left down) : a shell in the same container, with demo commands
#   dash    (right)     : /_lb/status refreshed every second
# Run from Git Bash on the host:  scripts/herdr_lab.sh [up|here|down]
#   up: a new tab   here: split the focused pane of the current tab   down: remove the container
# All panes share one long-lived container (mini-lab) so they see the same ports.
set -u
cd "$(dirname "$0")/.."
NAME=mini-lab IMAGE=mini-webserver-tools

pane() { # runs inside the container
    case $1 in
        backend)
            # A binary built in another environment (newer glibc) looks up to date to make.
            if [ -f webserver ] && ldd ./webserver 2>&1 | grep -q 'not found'; then make -s clean; fi
            scripts/lb.sh up || { echo "lb.sh up failed"; exec bash; }
            # The dash pane polls /_lb/status every second; hide those lines.
            tail -n 0 -F .lb/front.log .lb/b1.log .lb/b2.log .lb/b3.log | grep --line-buffered -v '/_lb/status'
            exec bash ;;
        dash)
            while :; do
                out=$(scripts/lb.sh status 2>&1)
                clear; date '+%T'; echo "$out"; sleep 1
            done ;;
        client)
            cat <<'EOF'
== client: try these (backend/dash panes show the effect)
  scripts/lb.sh keys 20            which backend each source IP sticks to
  scripts/lb.sh pause b2           freeze b2, then:  scripts/lb.sh resume b2
  scripts/lb.sh slow b2 500        b2 answers after 500 ms;  lb.sh start b2 restores it
  python3 tools/loadgen.py --clients 40 --duration 10 --path /echo --probe / --watch 127.0.0.1:8002
  make test                        unit tests
  bash scripts/functional_test.sh  e2e (uses :8080 too: run  scripts/lb.sh down  first)
EOF
            exec bash ;;
    esac
}

if [ "${1:-}" = pane ]; then pane "${2:-}"; exit; fi

MODE=${1:-up}
case $MODE in
    down) docker rm -f "$NAME" >/dev/null && echo "$NAME removed"; exit ;;
    up|here) ;;
    *) echo "usage: scripts/herdr_lab.sh [up|here|down]  (here: split the focused pane instead of a new tab)"; exit 1 ;;
esac

command -v herdr-app >/dev/null || { echo "herdr-app not on PATH"; exit 1; }
docker image inspect "$IMAGE" >/dev/null 2>&1 || docker build -q -t "$IMAGE" tools >/dev/null || exit 1
if [ "$(docker inspect -f '{{.State.Running}}' "$NAME" 2>/dev/null)" != true ]; then
    docker rm -f "$NAME" >/dev/null 2>&1
    # MSYS_NO_PATHCONV keeps Git Bash from rewriting /src.
    MSYS_NO_PATHCONV=1 docker run -d --name "$NAME" -v "$(pwd -W 2>/dev/null || pwd):/src" -w /src \
        "$IMAGE" sleep infinity >/dev/null || exit 1
fi

argv() { echo "[\"docker\",\"exec\",\"-it\",\"$NAME\",\"bash\",\"scripts/herdr_lab.sh\",\"pane\",\"$1\"]"; }
field() { grep -o "\"$1\": *\"[^\"]*\"" | head -1 | sed 's/.*"\([^"]*\)"$/\1/'; }

if [ "$MODE" = here ]; then
    focused=$(herdr-app status | field focused_pane)
    [ -n "$focused" ] || { echo "no focused pane in Herdr"; exit 1; }
    backend=$(herdr-app do pane.split --pane "$focused" --direction right --argv "$(argv backend)" | field pane)
else
    # tab.new reports the terminal, not the pane; herdr-app ls lists the pane right after it.
    term=$(herdr-app do tab.new --name lb-lab --argv "$(argv backend)" | field terminal)
    backend=$(herdr-app ls | grep -A1 "\"terminal\": \"$term\"" | field pane)
fi
[ -n "$backend" ] || { echo "could not find the backend pane"; exit 1; }
herdr-app do pane.split --pane "$backend" --direction right --argv "$(argv dash)" >/dev/null || exit 1
sleep 3   # dash needs the front up; give backend a head start before the client prompt
herdr-app do pane.split --pane "$backend" --direction down --argv "$(argv client)" >/dev/null || exit 1
echo "lb-lab panes opened (container $NAME). Close them, then: scripts/herdr_lab.sh down"
