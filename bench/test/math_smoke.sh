#!/usr/bin/env bash
# Unit 11 TDD smoke test (math, Cajeta side). Self-generating. Asserts the numeric
# kernels are ok (checksum / identity-matmul cross-checks) with input_size.
set -euo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
PROJ="$( cd -- "${SCRIPT_DIR}/.." &> /dev/null && pwd )"
REPO_ROOT="$( cd -- "${PROJ}/../.." &> /dev/null && pwd )"
CAJETA="${CAJETA:-${REPO_ROOT}/build-cajeta/src/cajeta}"

fail() { echo "MATH FAIL: $1" >&2; exit 1; }
[[ -x "$CAJETA" ]] || fail "cajeta not found at $CAJETA"

cd "$PROJ"
echo "[math] building (release)..."
"$CAJETA" release >/dev/null || fail "release build failed"

OUT="/tmp/profile-math.csv"
rm -f "$OUT"
PROFILE_RUN_ID=math-run "$PROJ/build/profile" --run --out "$OUT" >/tmp/math-run.log 2>&1 \
    || { grep -vE "example:" /tmp/math-run.log | tail -15; fail "--run crashed"; }
grep -E "measured (saxpy|dot-product|matmul)" /tmp/math-run.log | sed 's/^/  /'

for b in saxpy dot-product matmul; do
    row="$(awk -F, -v b="$b" 'NR>1 && $4==b {print; exit}' "$OUT")"
    [[ -n "$row" ]] || fail "no row for $b"
    st="$(printf '%s' "$row" | awk -F, '{print $28}')"
    [[ "$st" == "ok" ]] || fail "$b status=$st"
    echo "[math] $b ok  min_ns=$(printf '%s' "$row" | awk -F, '{print $17}')"
done

echo "MATH OK"
