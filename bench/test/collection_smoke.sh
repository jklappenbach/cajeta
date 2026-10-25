#!/usr/bin/env bash
# Unit 6 TDD smoke test (collections, Cajeta side) — written before the benchmarks.
# Collection benchmarks are self-generating (no dataset). Runs the release harness
# and asserts each collection benchmark is ok (its structural cross-check passed:
# count==N / value-sum closed form / dedup count / heap pop sorted) with input_size.
set -euo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
PROJ="$( cd -- "${SCRIPT_DIR}/.." &> /dev/null && pwd )"
REPO_ROOT="$( cd -- "${PROJ}/.." &> /dev/null && pwd )"
CAJETA="${CAJETA:-${REPO_ROOT}/build/src/cajeta}"

fail() { echo "COLLECTION FAIL: $1" >&2; exit 1; }
[[ -x "$CAJETA" ]] || fail "cajeta not found at $CAJETA"

cd "$PROJ"
echo "[collection] building (release)..."
"$CAJETA" release >/dev/null || fail "release build failed"

OUT="/tmp/profile-collection.csv"
rm -f "$OUT"
echo "[collection] running..."
CAJETA_BENCH_SMOKE=1 PROFILE_RUN_ID=coll-run "$PROJ/build/bench" --run --area collection --out "$OUT" >/tmp/coll-run.log 2>&1 \
    || { grep -vE "example:" /tmp/coll-run.log | tail -15; fail "--run crashed"; }
grep -E "^  (ok|skipped|invalid|failed)  .*/(arraylist|hashmap|hashset|heap|redblack)" /tmp/coll-run.log | sed 's/^/  /' || true

for b in arraylist-append hashmap-int hashmap-string hashset-dedup linkedlist-insert-traverse heap-sort redblacktree-insert-lookup; do
    row="$(awk -F, -v b="$b" 'NR>1 && $4==b {print; exit}' "$OUT")"
    [[ -n "$row" ]] || fail "no row for $b"
    st="$(printf '%s' "$row" | awk -F, '{print $28}')"
    ar="$(printf '%s' "$row" | awk -F, '{print $5}')"
    [[ "$ar" == "collection" ]] || fail "$b area=$ar (want collection; the id is collection/$b)"
    printf '%s' "$row" | awk -F, '{print $30}' | grep -q 'smoke' || fail "$b row not tagged smoke"
    isz="$(printf '%s' "$row" | awk -F, '{print $9}')"
    mn="$(printf '%s' "$row" | awk -F, '{print $17}')"
    [[ "$st" == "ok" ]] || fail "$b status=$st (cross-check failed)"
    [[ "$isz" =~ ^[0-9]+$ && "$isz" -gt 0 ]] || fail "$b input_size=$isz"
    echo "[collection] $b ok  n=$isz  min_ns=$mn"
done

echo "COLLECTION OK"
