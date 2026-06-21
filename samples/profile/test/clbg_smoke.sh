#!/usr/bin/env bash
# Unit 12 TDD smoke test (CLBG classics, Cajeta side). Self-generating. Asserts
# the compute classics are ok — their canonical checksums verify correctness:
# fannkuch (maxFlips=38/checksum=73196), spectral-norm (~1.274224), mandelbrot count.
set -euo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
PROJ="$( cd -- "${SCRIPT_DIR}/.." &> /dev/null && pwd )"
REPO_ROOT="$( cd -- "${PROJ}/../.." &> /dev/null && pwd )"
CAJETA="${CAJETA:-${REPO_ROOT}/build-cajeta/src/cajeta}"

fail() { echo "CLBG FAIL: $1" >&2; exit 1; }
[[ -x "$CAJETA" ]] || fail "cajeta not found at $CAJETA"

cd "$PROJ"
echo "[clbg] building (release)..."
"$CAJETA" release >/dev/null || fail "release build failed"

OUT="/tmp/profile-clbg.csv"
rm -f "$OUT"
PROFILE_RUN_ID=clbg-run "$PROJ/build/profile" --run --out "$OUT" >/tmp/clbg-run.log 2>&1 \
    || { grep -vE "example:" /tmp/clbg-run.log | tail -15; fail "--run crashed"; }
grep -E "measured clbg-" /tmp/clbg-run.log | sed 's/^/  /'

for b in clbg-mandelbrot clbg-fannkuch-redux clbg-spectral-norm; do
    row="$(awk -F, -v b="$b" 'NR>1 && $4==b {print; exit}' "$OUT")"
    [[ -n "$row" ]] || fail "no row for $b"
    st="$(printf '%s' "$row" | awk -F, '{print $28}')"
    [[ "$st" == "ok" ]] || fail "$b status=$st (canonical checksum mismatch)"
    echo "[clbg] $b ok  min_ns=$(printf '%s' "$row" | awk -F, '{print $17}')"
done

echo "CLBG OK"
