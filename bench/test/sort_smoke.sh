#!/usr/bin/env bash
# Unit 7 TDD smoke test (sorting, Cajeta side). Self-generating. Asserts the sort
# benchmarks are ok (non-decreasing / all-found cross-checks) — including the
# sort-int64 pattern matrix (random/ascending/descending/dups each a variant row).
# Sort moved to the collection/ package (spec §6.2).
set -euo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
source "$SCRIPT_DIR/csv_lib.sh"   # cawk / csv_fields: CSV-aware column checks
PROJ="$( cd -- "${SCRIPT_DIR}/.." &> /dev/null && pwd )"
REPO_ROOT="$( cd -- "${PROJ}/.." &> /dev/null && pwd )"
CAJETA="${CAJETA:-${REPO_ROOT}/build/src/cajeta}"

fail() { echo "SORT FAIL: $1" >&2; exit 1; }
[[ -x "$CAJETA" ]] || fail "cajeta not found at $CAJETA"

cd "$PROJ"
echo "[sort] building (release)..."
"$CAJETA" release >/dev/null || fail "release build failed"

OUT="/tmp/profile-sort.csv"
rm -f "$OUT"
CAJETA_BENCH_SMOKE=1 PROFILE_RUN_ID=sort-run "$PROJ/build/bench" --run --area collection --out "$OUT" >/tmp/sort-run.log 2>&1 \
    || { grep -vE "example:" /tmp/sort-run.log | tail -15; fail "--run crashed"; }
grep -E "^  (ok|skipped|invalid|failed)  .*/(sort|binary)" /tmp/sort-run.log | sed 's/^/  /' || true

col() { cawk -v b="$1" -v v="$2" -v c="$3" 'NR>1 && $4==b && $31==v {print $c; exit}' "$OUT"; }

# sort-int64: every pattern runs ok. The 0.7.1-era skip ("quicksort overflows
# its 128-deep range stack on adversarial patterns") is gone: Sort.sort recurses
# the smaller partition and loops the larger, so ascending/descending/dups sort
# correctly (re-tested at 0.46.3, benchmark-reference 2.2.4).
for p in random ascending descending dups; do
    st="$(col sort-int64 "$p" 28)"
    [[ "$st" == "ok" ]] || fail "sort-int64/$p status=$st"
    ar="$(col sort-int64 "$p" 5)"
    [[ "$ar" == "collection" ]] || fail "sort-int64/$p area=$ar"
    echo "[sort] sort-int64/$p ok  min_ns=$(col sort-int64 "$p" 17)"
done

# the rest (single variant)
for b in sort-f64 sort-stable-int64 binary-search; do
    st="$(cawk -v b="$b" 'NR>1 && $4==b {print $28; exit}' "$OUT")"
    [[ "$st" == "ok" ]] || fail "$b status=$st"
    echo "[sort] $b ok"
done

echo "SORT OK"
