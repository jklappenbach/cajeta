#!/usr/bin/env bash
# collection/ competitor smoke (plan 6.1.2): every program under
# competitors/drivers/collection/ (sort, lists, maps, ordered, cache, ltm) emits schema-v2 rows with area collection, and every id on
# bench/collection/*.md and ltm/LtmBPlusTree.md has rows in all five languages,
# each `ok` with check_ok=true or skipped for a reason this smoke allows:
#   cache-ttl  python (lru_cache has no TTL), cpp (hand-written LRU, no TTL),
#              rust lru::LruCache (no TTL; moka is the Rust TTL row)
#   ltm-*      cpp, go, python (the page measures RocksDB and LMDB through
#              their Rust and Java bindings only)
# Any other skip (a missing toolchain or pin on this machine) fails. The ltm
# rows record the storage device, and a run with an impossible disk budget
# turns them into skips that say so. The fast-hash competitors the HashMap
# page names (unordered_dense, hashbrown) must be present and ok.
#
# The programs run in parallel (each is sequential inside): smoke rows are
# never published, so their timings only have to exist, not be quiet.
# Env: PROFILE_WARMUP (default 1), PROFILE_TRIALS (default 3), CAJETA_BENCH_OUT,
#      COLL_COMP_ONLY="p ..." (re-run only these programs, keep the others' CSVs).
set -uo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
PROJ="$( cd -- "${SCRIPT_DIR}/.." &> /dev/null && pwd )"
COMP="$PROJ/competitors"
NCOLS="$(awk -F, '{print NF; exit}' "$COMP/columns.txt")"
OUTDIR="${CAJETA_BENCH_OUT:-$(mktemp -d)}/collection-competitors-smoke"
mkdir -p "$OUTDIR"
# CAJETA_BENCH_SMOKE=1: the first-suite rows run at n/100, as on the Cajeta side.
export CAJETA_BENCH_SMOKE=1 PROFILE_RUN_ID=coll-comp PROFILE_RUN_TS=0 PROFILE_WARMUP="${PROFILE_WARMUP:-1}" PROFILE_TRIALS="${PROFILE_TRIALS:-3}"

fail() { echo "COLL-COMP FAIL: $1" >&2; exit 1; }

PROGRAMS="$(cd "$COMP/drivers/collection" && ls *.sh | sed 's/\.sh$//' | tr '\n' ' ')"
RUN="${COLL_COMP_ONLY:-$PROGRAMS}"
echo "[coll-comp] programs: $PROGRAMS(parallel; logs in $OUTDIR)"
t0=$SECONDS
pids=()
for p in $RUN; do
    ( s=$SECONDS; bash "$COMP/run.sh" collection "$p" >"$OUTDIR/$p.csv" 2>"$OUTDIR/$p.err"; echo "$? $((SECONDS - s))" >"$OUTDIR/$p.rc" ) &
    pids+=($!)
done
wait "${pids[@]}"
for p in $PROGRAMS; do
    read -r rc dur <"$OUTDIR/$p.rc"
    printf '[coll-comp] %-10s rc=%s %ss %s rows\n' "$p" "$rc" "$dur" "$(wc -l <"$OUTDIR/$p.csv")"
    [[ "$rc" == 0 ]] || { tail -5 "$OUTDIR/$p.err"; fail "program $p exited $rc"; }
done
echo "[coll-comp] all programs took $((SECONDS - t0))s"
ALL="$OUTDIR/all.csv"
cat "$OUTDIR"/*.csv | grep -v '^$' >"$ALL" || true
[[ -s "$ALL" ]] || fail "no rows"

allowed_skip() {  # id lang lib
    case "$1" in
        cache-ttl) [[ "$2" == python || "$2" == cpp || "$3" == lru::LruCache ]] ;;
        ltm-insert|ltm-lookup) [[ "$2" == cpp || "$2" == go || "$2" == python ]] ;;
        *) return 1 ;;
    esac
}

# (1) Every row: schema v2, 37 columns, area collection, machine/os/platform set.
while IFS= read -r row; do
    [[ "$(awk -F, '{print NF}' <<<"$row")" == "$NCOLS" ]] || fail "row has the wrong column count: $row"
    awk -F, '$1!="2" || $5!="collection" || $33=="" || $34=="" || $37!=$10 {exit 1}' <<<"$row" \
        || fail "row is not v2/collection with machine, os and platform=language: $row"
    IFS=, read -r -a f <<<"$row"
    id="${f[3]}"; lang="${f[9]}"; lib="${f[11]}"; st="${f[27]}"; ck="${f[28]}"; note="${f[29]}"
    case "$st" in
        ok) [[ "$ck" == true && "${f[16]}" =~ ^[0-9]+$ && "${f[16]}" -gt 0 ]] || fail "ok row without a passing check or a time: $row" ;;
        skipped) [[ -n "$note" ]] || fail "skip without a reason: $row"
                 allowed_skip "$id" "$lang" "$lib" || fail "skip not allowed on this machine ($id $lang $lib): $note" ;;
        *) fail "status $st: $row" ;;
    esac
done <"$ALL"

# (2) Every page id has a row in each of the five languages.
PAGE_IDS="$(grep -ho '`collection/[a-z0-9-]*`' "$PROJ"/collection/*.md "$PROJ"/collection/ltm/*.md | tr -d '`' | sed 's#collection/##' | sort -u)"
for id in $PAGE_IDS; do
    for lang in cpp rust go java python; do
        awk -F, -v b="$id" -v l="$lang" '$4==b && $10==l' "$ALL" | grep -q . || fail "no $lang row for collection/$id"
    done
    n_ok="$(awk -F, -v b="$id" '$4==b && $28=="ok"' "$ALL" | wc -l)"
    printf '[coll-comp] %-28s %2s ok rows (%s)\n' "$id" "$n_ok" "$(awk -F, -v b="$id" '$4==b && $28=="ok" {print $10}' "$ALL" | sort -u | tr '\n' ' ')"
    [[ "$n_ok" -gt 0 ]] || fail "collection/$id has no ok competitor row"
done

# (3) The fast-hash libraries the HashMap page names, on every HashMap id that
# names them; the first-suite rows carry the smoke-size note.
for id in hashmap-int hashmap-string hashmap-lookup-hit; do
    for lib in ankerl::unordered_dense::map hashbrown::HashMap; do
        awk -F, -v b="$id" -v l="$lib" '$4==b && $12==l && $28=="ok"' "$ALL" | grep -q . || fail "$lib $id row missing"
    done
done
FIRST="arraylist-append linkedlist-insert-traverse hashmap-int hashmap-string hashset-dedup heap-sort redblacktree-insert-lookup sort-int64 sort-f64 sort-stable-int64 binary-search"
for id in $FIRST; do
    awk -F, -v b="$id" '$4==b && $28=="ok" && $30 !~ /smoke size n\/100/' "$ALL" | grep -q . && fail "$id has an ok row without the smoke-size note"
done

# (4) ltm rows record the storage device; without a disk budget they skip, saying so.
awk -F, '($4=="ltm-insert" || $4=="ltm-lookup") && $28=="ok" && $30 !~ /storage=/' "$ALL" | grep -q . && fail "an ltm row lacks storage="
NB="$(CAJETA_BENCH_LTM_MIN_FREE_MB=999999999 bash "$COMP/run.sh" collection ltm 2>"$OUTDIR/ltm-nobudget.err")"
for lang in rust java; do
    awk -F, -v l="$lang" '$10==l && $28=="skipped" && $30 ~ /disk budget unavailable/' <<<"$NB" | grep -q . \
        || fail "$lang ltm rows without a disk budget are not skips that say so: $NB"
done
echo "[coll-comp] ltm without a disk budget: rust and java rows skipped (disk budget unavailable)"

echo "COLL-COMP OK ($(wc -l <"$ALL") rows; $(echo $PAGE_IDS | wc -w) ids x 5 languages; csv $ALL)"
