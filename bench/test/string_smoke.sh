#!/usr/bin/env bash
# Unit 8 TDD smoke test (strings, Cajeta side). Self-generating. Asserts the
# string benchmarks are ok (length / found cross-checks) with input_size.
set -euo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
PROJ="$( cd -- "${SCRIPT_DIR}/.." &> /dev/null && pwd )"
REPO_ROOT="$( cd -- "${PROJ}/.." &> /dev/null && pwd )"
CAJETA="${CAJETA:-${REPO_ROOT}/build/src/cajeta}"

fail() { echo "STRING FAIL: $1" >&2; exit 1; }
[[ -x "$CAJETA" ]] || fail "cajeta not found at $CAJETA"

cd "$PROJ"
echo "[string] building (release)..."
"$CAJETA" release >/dev/null || fail "release build failed"

OUT="/tmp/profile-string.csv"
rm -f "$OUT"
CAJETA_BENCH_SMOKE=1 PROFILE_RUN_ID=string-run "$PROJ/build/bench" --run --area lang --out "$OUT" >/tmp/string-run.log 2>&1 \
    || { grep -vE "example:" /tmp/string-run.log | tail -15; fail "--run crashed"; }
grep -E "^  (ok|skipped|invalid|failed)  .*/string-" /tmp/string-run.log | sed 's/^/  /' || true

for b in string-build-concat string-search string-replace string-uppercase; do
    row="$(awk -F, -v b="$b" 'NR>1 && $4==b {print; exit}' "$OUT")"
    [[ -n "$row" ]] || fail "no row for $b"
    st="$(printf '%s' "$row" | awk -F, '{print $28}')"
    ar="$(printf '%s' "$row" | awk -F, '{print $5}')"
    [[ "$ar" == lang/* ]] || fail "$b area=$ar (want the lang/ catalog path)"
    printf '%s' "$row" | awk -F, '{print $30}' | grep -q 'smoke' || fail "$b row not tagged smoke"
    [[ "$st" == "ok" ]] || fail "$b status=$st"
    echo "[string] $b ok  n=$(printf '%s' "$row" | awk -F, '{print $9}')  min_ns=$(printf '%s' "$row" | awk -F, '{print $17}')"
done

echo "STRING OK"
