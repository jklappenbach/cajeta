#!/usr/bin/env bash
# Phase B sort smoke test — cross-language sort competitors over an identical
# splitmix64-generated 50K array. Asserts competitors/collection.sh sort emits schema-
# conformant sort-int64/sort-f64/sort-stable-int64 rows for the present languages
# (rust std-unstable/std-stable, cpp std::sort/pdqsort/std::stable_sort, go
# slices.Sort, python Timsort), that every measured row passes its
# sorted+sum-preserved cross-check, and — the diagnostic centerpiece — that the
# competitors COMPLETE the adversarial sort-int64 variants (ascending/descending/
# dups) which Cajeta's 128-deep quicksort stack skips. No dataset needed.
set -uo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
source "$SCRIPT_DIR/csv_lib.sh"   # cawk / csv_fields: CSV-aware column checks
PROJ="$( cd -- "${SCRIPT_DIR}/.." &> /dev/null && pwd )"
RUNNER="${PROJ}/competitors/collection.sh"   # program sort of package collection
NCOLS="$(cawk '{print NF; exit}' "${PROJ}/competitors/columns.txt")"

fail() { echo "SORT-COMP FAIL: $1" >&2; exit 1; }

echo "[sort-comp] building + running sort competitors (compiles cargo/g++/go on first run)…"
OUT="$(PROFILE_RUN_ID=sort-comp PROFILE_RUN_TS=0 PROFILE_TRIALS=3 bash "$RUNNER" sort 2>/tmp/sort-comp.err)" \
    || fail "sort.sh exited non-zero"
[[ -n "$OUT" ]] || fail "sort.sh produced no rows"

# (1) every row schema-conformant; benchmark in the expected set; area=sort.
while IFS= read -r row; do
    [[ -z "$row" ]] && continue
    N="$(printf '%s' "$row" | cawk '{print NF}')"
    [[ "$N" == "$NCOLS" ]] || fail "row has $N cols, expected $NCOLS: $row"
    [[ "$(printf '%s' "$row" | cawk '{print $1}')" == "2" ]] || fail "row is not schema_version 2: $row"
    printf '%s' "$row" | cawk '$33=="" || $34=="" || $37=="" || $37!=$10 {exit 1}' \
        || fail "row lacks machine/os/platform (platform must be the language): $row"
    bench="$(printf '%s' "$row" | cawk '{print $4}')"; area="$(printf '%s' "$row" | cawk '{print $5}')"
    case "$bench" in sort-int64|sort-f64|sort-stable-int64) ;; *) fail "unexpected benchmark: $bench" ;; esac
    [[ "$area" == "collection" ]] || fail "unexpected area: $area"
    status="$(printf '%s' "$row" | cawk '{print $28}')"
    if [[ "$status" == "ok" ]]; then
        min="$(printf '%s' "$row" | cawk '{print $17}')"
        chk="$(printf '%s' "$row" | cawk '{print $29}')"
        [[ "$min" =~ ^[0-9]+$ && "$min" -gt 0 ]] || fail "ok row min_ns not positive: $row"
        [[ "$chk" == "true" ]]                   || fail "ok row failed sorted/sum cross-check: $row"
    elif [[ "$status" == "skipped" ]]; then
        note="$(printf '%s' "$row" | cawk '{print $30}')"
        [[ -n "$note" ]] || fail "skip row missing reason: $row"
    else
        fail "unexpected status '$status': $row"
    fi
done < <(csv_fields <<<"$OUT")

# (2) the diagnostic: competitors complete the adversarial variants Cajeta skips.
#     The always-present languages (rust, python) must have ok rows for each.
for lang in rust python; do
    for v in ascending descending dups; do
        echo "$OUT" | cawk -v l="$lang" -v v="$v" \
            '$10==l && $4=="sort-int64" && $31==v && $28=="ok" && $29=="true"' | grep -q . \
            || fail "$lang did not complete adversarial sort-int64 variant '$v'"
    done
done

# (3) the variant column (col 31) is populated for sort-int64 with all four patterns.
VARIANTS="$(echo "$OUT" | cawk '$4=="sort-int64" && $28=="ok"{print $31}' | sort -u | tr '\n' ' ')"
for v in ascending descending dups random; do
    case " $VARIANTS " in *" $v "*) ;; *) fail "missing sort-int64 variant in results: $v" ;; esac
done


NLANGS="$(echo "$OUT" | cawk '$28=="ok"{print $10}' | sort -u | tr '\n' ' ')"
echo "[sort-comp] measured langs: [$NLANGS]; adversarial variants completed"
echo "SORT-COMP OK"
