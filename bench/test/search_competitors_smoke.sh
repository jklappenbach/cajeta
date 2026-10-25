#!/usr/bin/env bash
# search/ competitor smoke (benchmark-reference unit 13, plan 13.1.2): under
# CAJETA_BENCH_SMOKE=1, competitors/run.sh search emits schema-v2 rows with
# catalog ids search/<slug> for every id each language's page rows name:
#   rust, python, java  distance-levenshtein distance-jaro-winkler fuzzy-topk ngram-build ngram-query
#   go                  distance-levenshtein distance-jaro-winkler (bench/search/distance/Distance.md only)
# each with status ok, check_ok true and a positive min_ns, and the libraries
# the pages name (strsim, nucleo-matcher, rapidfuzz, commons-text, Lucene,
# agnivade/levenshtein). Every page id has at least one competitor row.
# Env: CAJETA_BENCH_OUT (log directory, default /tmp).
set -uo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
source "$SCRIPT_DIR/csv_lib.sh"   # cawk / csv_fields: CSV-aware column checks
PROJ="$( cd -- "${SCRIPT_DIR}/.." &> /dev/null && pwd )"
OUTD="${CAJETA_BENCH_OUT:-/tmp}"; mkdir -p "$OUTD"
NCOLS="$(cawk '{print NF; exit}' "${PROJ}/competitors/columns.txt")"
fail() { echo "SEARCH-COMP FAIL: $1" >&2; exit 1; }

echo "[search-comp] building + running rust python java go (smoke sizes)..."
t0=$SECONDS
OUT="$(cd "${PROJ}/competitors" && CAJETA_BENCH_SMOKE=1 PROFILE_RUN_ID=search-comp PROFILE_RUN_TS=0 PROFILE_TRIALS=3 PROFILE_WARMUP=1 \
        bash run.sh search 2>"$OUTD/search-comp.err")" || fail "run.sh search exited non-zero (see $OUTD/search-comp.err)"
echo "[search-comp] run $((SECONDS - t0)) s"
[[ -n "$OUT" ]] || fail "no rows"
printf '%s\n' "$OUT" > "$OUTD/search-comp.csv"

while IFS= read -r row; do
    [[ -z "$row" ]] && continue
    IFS="$CSV_FS" read -r -a c <<< "$row"
    N="$(cawk '{print NF}' <<< "$row")"
    [[ "$N" == "$NCOLS" && "${c[0]}" == "2" && "${c[4]}" == "search" ]] || fail "row not v2 search ($N cols): $row"
    [[ -n "${c[32]}" && -n "${c[33]}" && "${c[36]}" == "${c[9]}" ]] || fail "row lacks machine/os/platform: $row"
    [[ "${c[27]}" == "ok" && "${c[28]}" == "true" ]] || fail "${c[9]} ${c[11]} search/${c[3]} status=${c[27]} check=${c[28]} notes=${c[29]}"
    [[ "${c[16]}" =~ ^[0-9]+$ && "${c[16]}" -gt 0 ]] || fail "${c[9]} search/${c[3]} min_ns=${c[16]} (elided?)"
    [[ "${c[29]}" == *smoke* ]] || fail "${c[9]} search/${c[3]} notes do not name the smoke size"
    echo "[search-comp] ${c[9]} ${c[11]} search/${c[3]} ok  min_ns=${c[16]}"
done < <(csv_fields <<<"$OUT")

ALL="distance-levenshtein distance-jaro-winkler fuzzy-topk ngram-build ngram-query"
has() { cawk -v l="$1" -v b="$2" -v lib="${3:-}" '$10==l && $4==b && (lib=="" || $12==lib)' <<< "$OUT" | grep -q .; }
for lang in rust python java; do for b in $ALL; do has "$lang" "$b" || fail "no $lang search/$b row"; done; done
for b in distance-levenshtein distance-jaro-winkler; do has go "$b" || fail "no go search/$b row"; done
# the libraries the pages name
has rust distance-levenshtein strsim || fail "rust distance is not strsim"
has rust fuzzy-topk nucleo-matcher || fail "rust fuzzy-topk is not nucleo-matcher"
has python fuzzy-topk rapidfuzz || fail "python fuzzy-topk is not rapidfuzz"
has java distance-jaro-winkler commons-text || fail "java distance is not commons-text"
has java fuzzy-topk lucene-core || fail "java fuzzy-topk is not Lucene"
has go distance-levenshtein agnivade/levenshtein || fail "go distance is not agnivade/levenshtein"

# every id on the search pages has competitor rows
ids="$(grep -ho '^| `search/[a-z0-9-]*`' "$PROJ"/search/*/*.md | sed 's/.*search\///; s/`//' | sort -u)"
for id in $ids; do cawk -v b="$id" '$4==b' <<< "$OUT" | grep -q . || fail "page id search/$id has no competitor row"; done
echo "SEARCH-COMP OK ($(grep -c . <<< "$OUT") rows, $(echo "$ids" | wc -w) page ids)"
