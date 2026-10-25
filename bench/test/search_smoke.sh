#!/usr/bin/env bash
# search/ smoke (benchmark-reference unit 13, plan 13.1.1): distance over 1e6 pairs,
# fuzzy top-k, n-gram index build and query.
# Every id on the search pages (bench/search/**/*.md) emits a row per variant with status
# ok, check_ok true, a positive min_ns, the catalog id search/<slug> and the smoke tag,
# under CAJETA_BENCH_SMOKE=1 (reduced sizes; each benchmark says which in notes).
#
# Env: CAJETA (compiler, default <repo>/build/src/cajeta), CAJETA_BENCH_OUT
# (directory for the CSV and logs, default /tmp), NO_BUILD=1 (reuse build/bench).
set -euo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
source "$SCRIPT_DIR/csv_lib.sh"   # cawk / csv_fields: CSV-aware column checks
PROJ="$( cd -- "${SCRIPT_DIR}/.." &> /dev/null && pwd )"
REPO_ROOT="$( cd -- "${PROJ}/.." &> /dev/null && pwd )"
CAJETA="${CAJETA:-${REPO_ROOT}/build/src/cajeta}"
export CAJETA
OUTD="${CAJETA_BENCH_OUT:-/tmp}"; mkdir -p "$OUTD"

fail() { echo "SEARCH FAIL: $1" >&2; exit 1; }
[[ -x "$CAJETA" ]] || fail "cajeta not found at $CAJETA"

cd "$PROJ"
if [[ "${NO_BUILD:-0}" != 1 ]]; then
    echo "[search] building (release)..."
    t0=$SECONDS
    "$CAJETA" release >"$OUTD/search-build.log" 2>&1 || { tail -30 "$OUTD/search-build.log"; fail "release build failed"; }
    echo "[search] build $((SECONDS - t0)) s"
fi

OUT="$OUTD/cajeta-bench-search.csv"
LOG="$OUTD/search-run.log"
rm -f "$OUT"
t0=$SECONDS
CAJETA_BENCH_SMOKE=1 PROFILE_RUN_ID=search-run "$PROJ/build/bench" --run --area search --out "$OUT" >"$LOG" 2>&1 \
    || { tail -30 "$LOG"; fail "--run --area search exited non-zero"; }
echo "[search] run $((SECONDS - t0)) s"
grep -hE "^  (ok|skipped|invalid|failed)  " "$LOG" | sed 's/^/  /' || true

rows_for() { cawk -v b="$1" -v v="$2" 'NR>1 && $4==b && (v=="" || $31==v)' "$OUT"; }
check() {   # check <slug> [variant]
    local b="$1" v="${2:-}" rows row
    rows="$(rows_for "$b" "$v")"
    [[ -n "$rows" ]] || fail "no row for search/$b${v:+ [$v]}"
    while IFS= read -r row; do
        IFS="$CSV_FS" read -r -a c <<< "$row"
        [[ "${c[4]}/${c[3]}" == "search/$b" ]] || fail "$b id=${c[4]}/${c[3]} (want search/$b)"
        [[ "${c[27]}" == "ok" ]]   || fail "search/$b${v:+ [$v]} status=${c[27]} notes=${c[29]}"
        [[ "${c[28]}" == "true" ]] || fail "search/$b${v:+ [$v]} check_ok=${c[28]} notes=${c[29]}"
        [[ "${c[16]}" =~ ^[0-9]+$ && "${c[16]}" -gt 0 ]] || fail "search/$b${v:+ [$v]} min_ns=${c[16]} (elided?)"
        [[ "${c[29]}" == *smoke* ]] || fail "search/$b row not tagged smoke"
        echo "[search] search/$b${v:+ [$v]} ok  min_ns=${c[16]}"
    done <<< "$rows"
}
check_skip() {   # check_skip <slug> <variant>: a skipped row with a reason
    local row
    row="$(rows_for "$1" "$2")"
    [[ -n "$row" ]] || fail "no row for search/$1 [$2]"
    IFS="$CSV_FS" read -r -a c <<< "$row"
    [[ "${c[27]}" == "skipped" && -n "${c[29]}" ]] || fail "search/$1 [$2] status=${c[27]} (want skipped with a reason)"
    echo "[search] search/$1 [$2] skipped: ${c[29]}"
}

for b in distance-levenshtein distance-jaro-winkler fuzzy-topk ngram-build ngram-query; do check "$b"; done

# Every id on the pages is covered (a page row added later without a benchmark fails here).
ids="$(cat "$PROJ"/search/*.md $(ls "$PROJ"/search/*/*.md 2>/dev/null) | grep -o '^| `search/[a-z0-9-]*`' | sed 's/.*search\///; s/`//' | sort -u)"
for id in $ids; do
    cawk -v b="$id" 'NR>1 && $4==b' "$OUT" | grep -q . || fail "page id search/$id has no row"
done
others="$(cawk 'NR>1 && $5 != "search"' "$OUT" | wc -l)"
[[ "$others" == "0" ]] || fail "--area search emitted $others non-search rows"

echo "SEARCH OK ($(echo "$ids" | wc -w) page ids)"
