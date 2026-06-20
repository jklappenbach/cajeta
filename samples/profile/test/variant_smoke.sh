#!/usr/bin/env bash
# Unit 3 TDD smoke test (variant slice) — written before VariantBench/variants().
# `profile --run` should produce ONE result row per variant. Asserts the
# variant-demo benchmark yields two rows labelled small + large (variant col 31),
# both status ok (so prepare(variant) selected the right input and checkResult
# validated it).
set -euo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
PROJ="$( cd -- "${SCRIPT_DIR}/.." &> /dev/null && pwd )"
REPO_ROOT="$( cd -- "${PROJ}/../.." &> /dev/null && pwd )"
CAJETA="${CAJETA:-${REPO_ROOT}/build-cajeta/src/cajeta}"

fail() { echo "VARIANT FAIL: $1" >&2; exit 1; }
[[ -x "$CAJETA" ]] || fail "cajeta not found at $CAJETA"

cd "$PROJ"
echo "[variant] building..."
"$CAJETA" build >/dev/null || fail "build failed"

OUT="/tmp/profile-variant.csv"
rm -f "$OUT"
PROFILE_RUN_ID=variant-run "$PROJ/build/profile" --run --out "$OUT" || fail "--run exited non-zero"
[[ -f "$OUT" ]] || fail "results CSV not written"

# variant-demo rows: benchmark col 4, status col 28, variant col 31.
ROWS="$(awk -F, 'NR>1 && $4=="variant-demo"{print $31"|"$28}' "$OUT")"
echo "[variant] variant-demo rows (variant|status):"
printf '%s\n' "$ROWS"

COUNT="$(printf '%s\n' "$ROWS" | grep -c .)"
[[ "$COUNT" == "2" ]] || fail "expected 2 variant rows, got $COUNT"
printf '%s\n' "$ROWS" | grep -qx "small|ok"  || fail "missing small|ok row"
printf '%s\n' "$ROWS" | grep -qx "large|ok"  || fail "missing large|ok row"

echo "VARIANT OK"
