#!/usr/bin/env bash
# collection/ smoke (plan 6.1.1): every id on bench/collection/*.md and
# bench/collection/ltm/LtmBPlusTree.md emits a row under CAJETA_BENCH_SMOKE=1,
# area = collection, tagged smoke, with status ok and check_ok true, except the
# rows skipped by design, each with its reason:
#   redblacktree-range, bplustree-range   no range scan / cursor API in the stdlib
# Then the ltm rows: they record the storage device (notes storage=...), and a
# second run with an impossible disk budget turns both into skipped rows whose
# reason names the budget.
#
# Env: CAJETA (compiler, default <repo>/build/src/cajeta), NO_BUILD=1 (reuse
# build/bench), CAJETA_BENCH_OUT (directory for the CSVs, default a temp dir),
# CAJETA_BENCH_LTM_* (see src/main/cajeta/bench/collection/LtmEnv.cajeta).
set -euo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
PROJ="$( cd -- "${SCRIPT_DIR}/.." &> /dev/null && pwd )"
REPO_ROOT="$( cd -- "${PROJ}/.." &> /dev/null && pwd )"
CAJETA="${CAJETA:-${REPO_ROOT}/build/src/cajeta}"
OUTDIR="${CAJETA_BENCH_OUT:-$(mktemp -d)}/collection-smoke"
mkdir -p "$OUTDIR"

fail() { echo "COLLECTION FAIL: $1" >&2; exit 1; }

IDS_OK="arraylist-append arraylist-get arraylist-iterate arraylist-sort
linkedlist-insert-traverse
hashmap-int hashmap-string hashmap-lookup-hit hashmap-lookup-miss hashmap-iterate
hashset-dedup hashset-contains
heap-sort heap-push-pop
redblacktree-insert-lookup
bplustree-insert bplustree-point-lookup
cache-hit cache-miss cache-evict cache-ttl
immutablelist-build immutablelist-get immutablemap-build immutablemap-lookup
immutableset-build immutableset-contains
collectors-to-list collectors-group-by
sort-int64 sort-f64 sort-stable-int64 binary-search
ltm-insert ltm-lookup"
IDS_SKIP="redblacktree-range bplustree-range"

# Every id the pages name must be in one of the two lists (the pages are the contract).
PAGE_IDS="$(grep -ho '`collection/[a-z0-9-]*`' "$PROJ"/collection/*.md "$PROJ"/collection/ltm/*.md | tr -d '`' | sed 's#collection/##' | sort -u)"
for id in $PAGE_IDS; do
    case " $(echo $IDS_OK $IDS_SKIP) " in *" $id "*) ;; *) fail "page id collection/$id is not covered by this smoke" ;; esac
done

cd "$PROJ"
if [[ "${NO_BUILD:-0}" != 1 ]]; then
    [[ -x "$CAJETA" ]] || fail "cajeta not found at $CAJETA"
    echo "[collection] building (release)..."
    "$CAJETA" release >"$OUTDIR/build.log" 2>&1 || { tail -20 "$OUTDIR/build.log"; fail "release build failed"; }
fi

OUT="$OUTDIR/results.csv"
rm -f "$OUT"
echo "[collection] running --area collection (smoke)..."
t0=$SECONDS
CAJETA_BENCH_SMOKE=1 CAJETA_BENCH_RUN_ID=coll-smoke "$PROJ/build/bench" --run --area collection --out "$OUT" >"$OUTDIR/run.log" 2>&1 \
    || { tail -25 "$OUTDIR/run.log"; fail "--run exited non-zero"; }
echo "[collection] run took $((SECONDS - t0))s"

col() { awk -F, -v c="$1" '{print $c}'; }
row_of() { awk -F, -v b="$1" 'NR>1 && $4==b {print; exit}' "$OUT"; }

for b in $IDS_OK; do
    row="$(row_of "$b")"
    [[ -n "$row" ]] || fail "no row for collection/$b"
    [[ "$(col 5 <<<"$row")" == collection ]] || fail "$b area=$(col 5 <<<"$row")"
    col 30 <<<"$row" | grep -q smoke || fail "$b row not tagged smoke"
    st="$(col 28 <<<"$row")"; ck="$(col 29 <<<"$row")"
    [[ "$st" == ok && "$ck" == true ]] || fail "$b status=$st check_ok=$ck notes=$(col 30 <<<"$row")"
    isz="$(col 9 <<<"$row")"
    [[ "$isz" =~ ^[0-9]+$ && "$isz" -gt 0 ]] || fail "$b input_size=$isz"
    printf '[collection] %-28s ok  median_ns=%s\n' "$b" "$(col 18 <<<"$row")"
done
for b in $IDS_SKIP; do
    row="$(row_of "$b")"
    [[ -n "$row" ]] || fail "no row for collection/$b"
    [[ "$(col 28 <<<"$row")" == skipped ]] || fail "$b status=$(col 28 <<<"$row") (expected skipped)"
    col 30 <<<"$row" | grep -q 'no range scan' || fail "$b skip reason missing: $(col 30 <<<"$row")"
    printf '[collection] %-28s skipped (%s)\n' "$b" "$(col 30 <<<"$row" | cut -c1-70)"
done
# The first-suite rows run at n/100 under smoke and say so.
for b in arraylist-append linkedlist-insert-traverse hashmap-int hashmap-string hashset-dedup heap-sort \
         redblacktree-insert-lookup sort-int64 sort-f64 sort-stable-int64 binary-search; do
    col 30 <<<"$(row_of "$b")" | grep -q 'smoke size n/100' || fail "$b row lacks the smoke-size note"
done
for b in ltm-insert ltm-lookup; do
    col 30 <<<"$(row_of "$b")" | grep -q 'storage=' || fail "$b does not record the storage device"
done

# The disk budget unavailable: both ltm rows skip with the reason.
OUT2="$OUTDIR/ltm-nobudget.csv"
rm -f "$OUT2"
CAJETA_BENCH_SMOKE=1 CAJETA_BENCH_LTM_MIN_FREE_MB=999999999 "$PROJ/build/bench" --run --area collection --bench ltm-insert --out "$OUT2" >"$OUTDIR/run2.log" 2>&1 \
    || { tail -10 "$OUTDIR/run2.log"; fail "no-budget run exited non-zero"; }
row="$(awk -F, 'NR>1 && $4=="ltm-insert" {print; exit}' "$OUT2")"
[[ "$(col 28 <<<"$row")" == skipped ]] || fail "ltm-insert without disk budget: status=$(col 28 <<<"$row")"
col 30 <<<"$row" | grep -q 'disk budget unavailable' || fail "ltm-insert skip reason: $(col 30 <<<"$row")"
echo "[collection] ltm-insert without a disk budget: skipped ($(col 30 <<<"$row" | cut -c1-80))"

echo "COLLECTION OK ($(echo $IDS_OK | wc -w) ok, $(echo $IDS_SKIP | wc -w) skipped by design; csv $OUT)"
