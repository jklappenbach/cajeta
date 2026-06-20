#!/usr/bin/env bash
# Unit 3 TDD smoke test (measurement loop slice) — written before Harness.
# Drives `profile --run --out <path>`, which measures every registered benchmark
# through the warmup+trials loop and writes results CSV rows. Asserts the SumBench
# row: trials honored, min_ns positive, median in [min,p95], status ok, check_ok,
# and a working_set_kb present.
set -euo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
PROJ="$( cd -- "${SCRIPT_DIR}/.." &> /dev/null && pwd )"
REPO_ROOT="$( cd -- "${PROJ}/../.." &> /dev/null && pwd )"
CAJETA="${CAJETA:-${REPO_ROOT}/build-cajeta/src/cajeta}"

fail() { echo "METHOD FAIL: $1" >&2; exit 1; }
[[ -x "$CAJETA" ]] || fail "cajeta not found at $CAJETA"

cd "$PROJ"
echo "[method] building..."
"$CAJETA" build >/dev/null || fail "build failed"

OUT="/tmp/profile-method.csv"
rm -f "$OUT"
echo "[method] running --run..."
PROFILE_RUN_ID=method-run "$PROJ/build/profile" --run --out "$OUT" || fail "--run exited non-zero"
[[ -f "$OUT" ]] || fail "results CSV not written"

# Pull the SumBench row (benchmark col 4 == "sum").
ROW="$(awk -F, 'NR>1 && $4=="sum"{print; exit}' "$OUT")"
[[ -n "$ROW" ]] || fail "no sum row in CSV"
echo "[method] sum row: $ROW"

get() { echo "$ROW" | awk -F, -v c="$1" '{print $c}'; }
TRIALS="$(get 16)"; MIN="$(get 17)"; MED="$(get 18)"; MEAN="$(get 19)"; P95="$(get 20)"
WS="$(get 27)"; STATUS="$(get 28)"; CHECK="$(get 29)"

[[ "$TRIALS" == "50" ]]            || fail "trials expected 50, got $TRIALS"
[[ "$MIN" =~ ^[0-9]+$ && "$MIN" -gt 0 ]] || fail "min_ns not positive: $MIN"
[[ "$MED" =~ ^[0-9]+$ ]]           || fail "median_ns not int: $MED"
[[ "$P95" =~ ^[0-9]+$ ]]           || fail "p95_ns not int: $P95"
[[ "$MIN" -le "$MED" ]]            || fail "min > median ($MIN > $MED)"
[[ "$MED" -le "$P95" ]]            || fail "median > p95 ($MED > $P95)"
[[ "$WS" =~ ^[0-9]+$ ]]            || fail "working_set_kb not int: $WS"
[[ "$STATUS" == "ok" ]]           || fail "status not ok: $STATUS"
[[ "$CHECK" == "true" ]]          || fail "check_ok not true: $CHECK"

echo "[method] trials=$TRIALS min=$MIN median=$MED mean=$MEAN p95=$P95 ws_kb=$WS"
echo "METHOD OK"
