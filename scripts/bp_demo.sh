#!/usr/bin/env bash
# Back-pressure comparison: the same load and the same failure of one target backend, run once per
# back-pressure cap, with a fresh lb.sh up before every run.
#   scripts/bp_demo.sh [--scenarios hung,slow,normal] [--caps 0,2,1] [--duration 10] [--clients 40]
#                      [--delay 500] [--backends 3] [--target b2] [--weights 1,1,1] [--timeline]
# hung = target paused, slow = target answers after --delay ms, normal = no failure.
# --backends N (1..8) and --weights (one 1..4 per backend) go to lb.sh up as LB_BACKENDS / LB_WEIGHTS.
# cap 0 also sets LB_WAIT=0 (back-pressure fully off). Front proxy timeout is 3 s (LB_TIMEOUT=3000);
# a static probe slower than 1 s (STATIC_SLO) counts as static err. Results: .lb/run-<scenario>-<cap>.json
# moved = clients that later hit a backend other than the one in their first named response.
# In hung, the target is paused before load starts, so moves away from it are not counted there.
set -u
cd "$(dirname "$0")/.."
SCENARIOS=hung,slow,normal CAPS=0,2,1 DURATION=10 CLIENTS=40 DELAY=500 TIMELINE= NBACK=3 TARGET=b2 WEIGHTS= WSET=
WAIT_DEFAULT=${LB_WAIT:-2000} STATIC_SLO=1
export LB_TIMEOUT=3000

usage() { sed -n '4,5p' "$0" | sed 's/^# *//'; exit 1; }
die() { echo "$*" >&2; exit 1; }
# num OPTION VALUE MIN MAX: prints VALUE as a canonical decimal (so 00 -> 0) or exits.
num() {
    case $2 in ''|*[!0-9]*) die "$1 needs a number, got '$2'" ;; esac
    [ ${#2} -le 9 ] && [ $((10#$2)) -ge "$3" ] && [ $((10#$2)) -le "$4" ] || die "$1 must be $3..$4, got '$2'"
    echo $((10#$2))
}

while [ $# -gt 0 ]; do
    case $1 in
        --scenarios) SCENARIOS=${2:-}; shift ;;
        --caps)      CAPS=${2:-}; shift ;;
        --duration)  DURATION=$(num "$1" "${2:-}" 1 3600) || exit 1; shift ;;
        --clients)   CLIENTS=$(num "$1" "${2:-}" 1 250) || exit 1; shift ;;
        --delay)     DELAY=$(num "$1" "${2:-}" 1 600000) || exit 1; shift ;;
        --timeline)  TIMELINE=--timeline ;;
        --backends)  NBACK=$(num "$1" "${2:-}" 1 8) || exit 1; shift ;;
        --target)    TARGET=${2:-}; shift ;;
        --weights)   [ $# -ge 2 ] || die "--weights needs a value such as 1,1,1"; WEIGHTS=$2; WSET=1; shift ;;
        *)           usage ;;
    esac
    shift
done
[ -n "${SCENARIOS//,/}" ] || die "--scenarios needs a list such as hung,slow,normal"
[ -n "${CAPS//,/}" ] || die "--caps needs a list such as 0,2,1"
for s in ${SCENARIOS//,/ }; do case $s in hung|slow|normal) ;; *) die "unknown scenario '$s'" ;; esac; done
CAPLIST=
for c in ${CAPS//,/ }; do CAPLIST+="$(num --caps "$c" 0 1024) " || exit 1; done
case $TARGET in b[1-9]) ;; *) die "--target must be b1..b$NBACK, got '$TARGET'" ;; esac
[ "${TARGET#b}" -le "$NBACK" ] || die "--target must be b1..b$NBACK, got '$TARGET'"
ONES=$(seq "$NBACK" | sed 's/.*/1/' | paste -sd,)
if [ -n "$WSET" ]; then
    # One line of N weights: an empty value, a newline or a wrong count is refused here, before lb.sh down.
    [[ $WEIGHTS =~ ^[1-4](,[1-4])*$ ]] && [ "$(awk -F, '{print NF}' <<<"$WEIGHTS")" = "$NBACK" ] ||
        die "--weights needs $NBACK values of 1..4 such as $ONES, got '$WEIGHTS'"
else
    WEIGHTS=$ONES
fi
# lb.sh up takes the setup from these (bp_demo always starts from down, so no old members are kept).
export LB_BACKENDS=$NBACK LB_WEIGHTS=$WEIGHTS

LB=scripts/lb.sh
# down resumes a paused backend before stopping it, so the target is never left frozen.
trap '$LB down >/dev/null 2>&1' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

make -s all || exit 1
mkdir -p .lb
RUNS=() FAILED=0 n=0
echo "clients $CLIENTS  duration ${DURATION}s  delay ${DELAY}ms  timeout ${LB_TIMEOUT}ms  static SLO ${STATIC_SLO}s"
for s in ${SCENARIOS//,/ }; do
    for c in $CAPLIST; do
        n=$((n + 1))
        out=.lb/run-$n-$s-$c                        # the index keeps repeated entries apart
        rm -f "$out.json"
        $LB down >/dev/null
        export LB_CAP=$c
        if [ "$c" = 0 ]; then export LB_WAIT=0; else export LB_WAIT=$WAIT_DEFAULT; fi
        $LB up >"$out.setup" 2>&1 || { echo "$s cap $c: lb.sh up failed, see $out.setup"; exit 1; }
        case $s in
            hung) $LB pause "$TARGET" >>"$out.setup" ;;
            slow) $LB slow "$TARGET" "$DELAY" >>"$out.setup" || { echo "$s cap $c: lb.sh slow failed"; exit 1; } ;;
        esac
        printf '%-7s cap %-3s %s ... ' "$s" "$c" "$(head -c 200 .lb/front.opts)"
        rc=0
        python3 tools/loadgen.py --clients "$CLIENTS" --duration "$DURATION" --path /echo --probe / --probe-timeout "$STATIC_SLO" \
            --watch "127.0.0.1:$((8000 + ${TARGET#b}))" --json "$out.json" $TIMELINE >"$out.txt" 2>&1 || rc=$?
        $LB down >/dev/null
        if [ "$rc" = 0 ] && python3 -c 'import json,sys; e=json.load(open(sys.argv[1]))["echo"]
print("echo %d req, %s" % (e["requests"], " ".join("%s:%d" % kv for kv in sorted(e["codes"].items()))))' "$out.json" 2>/dev/null; then
            :
        else
            echo "FAILED (loadgen exit $rc), see $out.txt"
            FAILED=1
            rm -f "$out.json"
        fi
        RUNS+=("$s" "$c" "$out")
    done
done

if [ -n "$TIMELINE" ]; then
    for ((i = 0; i < ${#RUNS[@]}; i += 3)); do
        echo; echo "== ${RUNS[i]} cap ${RUNS[i+1]}"; cat "${RUNS[i+2]}.txt"
    done
fi
echo
echo "backends $NBACK  weights $WEIGHTS  target $TARGET"
python3 -c '
import json, sys
a = sys.argv[1:]
ms = lambda v: "-" if v is None else "%.0fms" % v
rows = [("scenario", "cap", "static p99", "static err", "echo 200", "503", "504", "err", "echo p99", "moved")]
for s, c, f in zip(a[0::3], a[1::3], a[2::3]):
    try:
        r = json.load(open(f + ".json"))
    except OSError:
        rows.append((s, c, "FAILED") + ("",) * 7)
        continue
    e, p, w = r["echo"], r["probe"], r.get("watch") or {}
    perr = p["err"] + sum(n for k, n in p["codes"].items() if k != "200")
    rows.append((s, c, ms(p["p99_ms"]), str(perr), str(e["codes"].get("200", 0)), str(e["codes"].get("503", 0)),
                 str(e["codes"].get("504", 0)), str(e["err"]), ms(e["p99_ms"]), str(e.get("moved", 0)),
                 "*health" if w.get("down_seen") else ""))
width = [max(len(r[i]) for r in rows) for i in range(10)]
for r in rows:
    line = "  ".join(r[i].ljust(width[i]) if i == 0 else r[i].rjust(width[i]) for i in range(10))
    print((line + "  " + r[10]) if len(r) > 10 and r[10] else line)
' "${RUNS[@]}"
exit $FAILED
