#!/usr/bin/env bash
# Unit 13 TDD smoke test (time & date, Cajeta side). Self-generating. Asserts the
# time arithmetic benchmarks are ok (closed-form epoch-sum cross-checks).
set -euo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
PROJ="$( cd -- "${SCRIPT_DIR}/.." &> /dev/null && pwd )"
REPO_ROOT="$( cd -- "${PROJ}/../.." &> /dev/null && pwd )"
CAJETA="${CAJETA:-${REPO_ROOT}/build-cajeta/src/cajeta}"

fail() { echo "TIME FAIL: $1" >&2; exit 1; }
[[ -x "$CAJETA" ]] || fail "cajeta not found at $CAJETA"

cd "$PROJ"
echo "[time] building (release)..."
"$CAJETA" release >/dev/null || fail "release build failed"

OUT="/tmp/profile-time.csv"
rm -f "$OUT"
PROFILE_RUN_ID=time-run "$PROJ/build/profile" --run --out "$OUT" >/tmp/time-run.log 2>&1 \
    || { grep -vE "example:" /tmp/time-run.log | tail -15; fail "--run crashed"; }
grep -E "measured time-" /tmp/time-run.log | sed 's/^/  /'

for b in time-instant-arith time-localdate-arith; do
    row="$(awk -F, -v b="$b" 'NR>1 && $4==b {print; exit}' "$OUT")"
    [[ -n "$row" ]] || fail "no row for $b"
    st="$(printf '%s' "$row" | awk -F, '{print $28}')"
    [[ "$st" == "ok" ]] || fail "$b status=$st"
    echo "[time] $b ok  min_ns=$(printf '%s' "$row" | awk -F, '{print $17}')"
done

echo "TIME OK"
