#!/usr/bin/env bash
# Unit 14 TDD smoke test (concurrency, Cajeta side). Self-generating. Asserts the
# atomic-counter and task-spawn/await benchmarks are ok (final==N / sum cross-checks).
set -euo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
PROJ="$( cd -- "${SCRIPT_DIR}/.." &> /dev/null && pwd )"
REPO_ROOT="$( cd -- "${PROJ}/../.." &> /dev/null && pwd )"
CAJETA="${CAJETA:-${REPO_ROOT}/build-cajeta/src/cajeta}"

fail() { echo "CONCURRENT FAIL: $1" >&2; exit 1; }
[[ -x "$CAJETA" ]] || fail "cajeta not found at $CAJETA"

cd "$PROJ"
echo "[concurrent] building (release)..."
"$CAJETA" release >/dev/null || fail "release build failed"

OUT="/tmp/profile-concurrent.csv"
rm -f "$OUT"
PROFILE_RUN_ID=conc-run "$PROJ/build/profile" --run --out "$OUT" >/tmp/conc-run.log 2>&1 \
    || { grep -vE "example:" /tmp/conc-run.log | tail -15; fail "--run crashed"; }
grep -E "measured (atomic|task)" /tmp/conc-run.log | sed 's/^/  /'

for b in atomic-fetchadd task-spawn-await; do
    row="$(awk -F, -v b="$b" 'NR>1 && $4==b {print; exit}' "$OUT")"
    [[ -n "$row" ]] || fail "no row for $b"
    st="$(printf '%s' "$row" | awk -F, '{print $28}')"
    [[ "$st" == "ok" ]] || fail "$b status=$st"
    echo "[concurrent] $b ok  min_ns=$(printf '%s' "$row" | awk -F, '{print $17}')"
done

echo "CONCURRENT OK"
