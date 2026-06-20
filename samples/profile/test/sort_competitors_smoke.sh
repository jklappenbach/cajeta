#!/usr/bin/env bash
# Phase B sort smoke test — cross-language sort competitors over an identical
# splitmix64-generated 50K array. Asserts competitors/sort.sh emits schema-
# conformant sort-int64/sort-f64/sort-stable-int64 rows for the present languages
# (rust std-unstable/std-stable, cpp std::sort/pdqsort/std::stable_sort, go
# slices.Sort, python Timsort), that every measured row passes its
# sorted+sum-preserved cross-check, and — the diagnostic centerpiece — that the
# competitors COMPLETE the adversarial sort-int64 variants (ascending/descending/
# dups) which Cajeta's 128-deep quicksort stack skips. No dataset needed.
set -uo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
PROJ="$( cd -- "${SCRIPT_DIR}/.." &> /dev/null && pwd )"
RUNNER="${PROJ}/competitors/sort.sh"
NCOLS="$(awk -F, '{print NF; exit}' "${PROJ}/competitors/columns.txt")"

fail() { echo "SORT-COMP FAIL: $1" >&2; exit 1; }

echo "[sort-comp] building + running sort competitors (compiles cargo/g++/go on first run)…"
OUT="$(PROFILE_RUN_ID=sort-comp PROFILE_RUN_TS=0 PROFILE_TRIALS=3 bash "$RUNNER" 2>/tmp/sort-comp.err)" \
    || fail "sort.sh exited non-zero"
[[ -n "$OUT" ]] || fail "sort.sh produced no rows"

# (1) every row schema-conformant; benchmark in the expected set; area=sort.
while IFS= read -r row; do
    [[ -z "$row" ]] && continue
    N="$(printf '%s' "$row" | awk -F, '{print NF}')"
    [[ "$N" == "$NCOLS" ]] || fail "row has $N cols, expected $NCOLS: $row"
    bench="$(printf '%s' "$row" | awk -F, '{print $4}')"; area="$(printf '%s' "$row" | awk -F, '{print $5}')"
    case "$bench" in sort-int64|sort-f64|sort-stable-int64) ;; *) fail "unexpected benchmark: $bench" ;; esac
    [[ "$area" == "sort" ]] || fail "unexpected area: $area"
    status="$(printf '%s' "$row" | awk -F, '{print $28}')"
    if [[ "$status" == "ok" ]]; then
        min="$(printf '%s' "$row" | awk -F, '{print $17}')"
        chk="$(printf '%s' "$row" | awk -F, '{print $29}')"
        [[ "$min" =~ ^[0-9]+$ && "$min" -gt 0 ]] || fail "ok row min_ns not positive: $row"
        [[ "$chk" == "true" ]]                   || fail "ok row failed sorted/sum cross-check: $row"
    elif [[ "$status" == "skipped" ]]; then
        note="$(printf '%s' "$row" | awk -F, '{print $30}')"
        [[ -n "$note" ]] || fail "skip row missing reason: $row"
    else
        fail "unexpected status '$status': $row"
    fi
done <<< "$OUT"

# (2) the diagnostic: competitors complete the adversarial variants Cajeta skips.
#     The always-present languages (rust, python) must have ok rows for each.
for lang in rust python; do
    for v in ascending descending dups; do
        echo "$OUT" | awk -F, -v l="$lang" -v v="$v" \
            '$10==l && $4=="sort-int64" && $31==v && $28=="ok" && $29=="true"' | grep -q . \
            || fail "$lang did not complete adversarial sort-int64 variant '$v'"
    done
done

# (3) the variant column (col 31) is populated for sort-int64 with all four patterns.
VARIANTS="$(echo "$OUT" | awk -F, '$4=="sort-int64" && $28=="ok"{print $31}' | sort -u | tr '\n' ' ')"
for v in ascending descending dups random; do
    case " $VARIANTS " in *" $v "*) ;; *) fail "missing sort-int64 variant in results: $v" ;; esac
done

# (4) report.py ingests the sort competitor rows.
TMP="$(mktemp -d)"
{ cat "${PROJ}/competitors/columns.txt"; echo "$OUT"; } > "$TMP/results.csv"
echo "schema_version,key,value" > "$TMP/env.csv"
python3 "${PROJ}/report/report.py" "$TMP" >/dev/null 2>&1 || fail "report.py failed on sort rows"
grep -q "sort-int64" "$TMP/site/index.html" || fail "report site missing sort-int64 rows"
rm -rf "$TMP"

NLANGS="$(echo "$OUT" | awk -F, '$28=="ok"{print $10}' | sort -u | tr '\n' ' ')"
echo "[sort-comp] measured langs: [$NLANGS]; adversarial variants completed (Cajeta skips them)"
echo "SORT-COMP OK"
