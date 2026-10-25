#!/usr/bin/env bash
# time/ smoke (Cajeta side; benchmark-reference unit 8, spec §6.4). Builds the
# release harness, runs `--area time` under CAJETA_BENCH_SMOKE=1, and asserts
# every id on bench/time/*.md emits a row in area `time`, tagged `smoke`, with
# status ok and check_ok true. The one recorded exception is
# time/datetime-parse-iso: cajeta.time has no parse API today, so its row must
# be `skipped` with that reason (never a silent omission).
# Anti-elision: an ok row of N operations must take at least N/10 ns.
#
# Env: CAJETA (compiler), CAJETA_BENCH_OUT (directory for the CSV and log;
# default /tmp), NO_BUILD=1 (reuse build/bench).
set -euo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
PROJ="$( cd -- "${SCRIPT_DIR}/.." &> /dev/null && pwd )"
REPO_ROOT="$( cd -- "${PROJ}/.." &> /dev/null && pwd )"
CAJETA="${CAJETA:-${REPO_ROOT}/build/src/cajeta}"
OUTDIR="${CAJETA_BENCH_OUT:-/tmp}"; mkdir -p "$OUTDIR"

fail() { echo "TIME FAIL: $1" >&2; exit 1; }
[[ -x "$CAJETA" ]] || fail "cajeta not found at $CAJETA"

cd "$PROJ"
if [[ "${NO_BUILD:-0}" != 1 ]]; then
    echo "[time] building (release)..."
    t0=$SECONDS
    "$CAJETA" release >"$OUTDIR/time-build.log" 2>&1 || { tail -30 "$OUTDIR/time-build.log"; fail "release build failed"; }
    echo "[time] built in $((SECONDS - t0))s"
fi

OUT="$OUTDIR/profile-time.csv"
LOG="$OUTDIR/time-run.log"
rm -f "$OUT"
t0=$SECONDS
CAJETA_BENCH_SMOKE=1 PROFILE_RUN_ID=time-run "$PROJ/build/bench" --run --area time --out "$OUT" >"$LOG" 2>&1 \
    || { grep -vE "example:" "$LOG" | tail -15; fail "--run exited non-zero"; }
echo "[time] ran in $((SECONDS - t0))s"

# id  operations-per-trial (for the anti-elision floor)
while read -r b ops; do
    [[ -z "$b" ]] && continue
    row="$(awk -F, -v b="$b" 'NR>1 && $4==b {print; exit}' "$OUT")"
    [[ -n "$row" ]] || fail "no row for time/$b"
    st="$(printf '%s' "$row" | awk -F, '{print $28}')"
    ar="$(printf '%s' "$row" | awk -F, '{print $5}')"
    [[ "$ar" == "time" ]] || fail "$b area=$ar (want time; the id is time/$b)"
    printf '%s' "$row" | awk -F, '{print $30}' | grep -q 'smoke' || fail "$b row not tagged smoke"
    if [[ "$b" == "datetime-parse-iso" ]]; then
        [[ "$st" == "skipped" ]] || fail "$b status=$st (cajeta.time has no parse API: expected a skip; if it now parses, make this an ok row)"
        printf '%s' "$row" | awk -F, '{print $30}' | grep -q 'parse' || fail "$b skip row lacks its reason"
        echo "[time] $b skipped (no parse API in cajeta.time)"
        continue
    fi
    [[ "$st" == "ok" ]] || fail "$b status=$st notes=$(printf '%s' "$row" | awk -F, '{print $30}')"
    [[ "$(printf '%s' "$row" | awk -F, '{print $29}')" == "true" ]] || fail "$b check_ok not true"
    min="$(printf '%s' "$row" | awk -F, '{print $17}')"
    (( min >= ops / 10 )) || fail "$b min_ns=$min for $ops operations looks elided"
    echo "[time] $b ok  min_ns=$min"
done <<'IDS'
time-instant-arith 10000000
time-localdate-arith 10000000
clock-now 10000000
duration-arith 10000000
localtime-arith 10000000
localdatetime-arith 10000000
zoneddatetime-region 1000000
datetime-format-iso 1000000
datetime-parse-iso 1000000
IDS

echo "TIME OK"
