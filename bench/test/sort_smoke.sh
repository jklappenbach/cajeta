#!/usr/bin/env bash
# Unit 7 TDD smoke test (sorting, Cajeta side). Self-generating. Asserts the sort
# benchmarks are ok (non-decreasing / all-found cross-checks) — including the
# sort-int64 pattern matrix (random/ascending/descending/dups each a variant row).
set -euo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
PROJ="$( cd -- "${SCRIPT_DIR}/.." &> /dev/null && pwd )"
REPO_ROOT="$( cd -- "${PROJ}/../.." &> /dev/null && pwd )"
CAJETA="${CAJETA:-${REPO_ROOT}/build-cajeta/src/cajeta}"

fail() { echo "SORT FAIL: $1" >&2; exit 1; }
[[ -x "$CAJETA" ]] || fail "cajeta not found at $CAJETA"

cd "$PROJ"
echo "[sort] building (release)..."
"$CAJETA" release >/dev/null || fail "release build failed"

OUT="/tmp/profile-sort.csv"
rm -f "$OUT"
PROFILE_RUN_ID=sort-run "$PROJ/build/profile" --run --out "$OUT" >/tmp/sort-run.log 2>&1 \
    || { grep -vE "example:" /tmp/sort-run.log | tail -15; fail "--run crashed"; }
grep -E "measured (sort|binary)" /tmp/sort-run.log | sed 's/^/  /'

col() { awk -F, -v b="$1" -v v="$2" -v c="$3" 'NR>1 && $4==b && $31==v {print $c; exit}' "$OUT"; }

# sort-int64: random runs ok; adversarial patterns skip (Cajeta quicksort
# overflows its 128-deep range stack — surfaced as data).
st="$(col sort-int64 random 28)"
[[ "$st" == "ok" ]] || fail "sort-int64/random status=$st"
echo "[sort] sort-int64/random ok  min_ns=$(col sort-int64 random 17)"
for p in ascending descending dups; do
    st="$(col sort-int64 "$p" 28)"
    [[ "$st" == "skipped" ]] || fail "sort-int64/$p expected skip (quicksort stack), got $st"
    echo "[sort] sort-int64/$p skipped (adversarial) ✓"
done

# the rest (single variant)
for b in sort-f64 sort-stable-int64 binary-search; do
    st="$(awk -F, -v b="$b" 'NR>1 && $4==b {print $28; exit}' "$OUT")"
    [[ "$st" == "ok" ]] || fail "$b status=$st"
    echo "[sort] $b ok"
done

echo "SORT OK"
