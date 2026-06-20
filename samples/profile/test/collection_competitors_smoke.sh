#!/usr/bin/env bash
# Phase B collection smoke test — cross-language hashmap-int / hashmap-string /
# hashset-dedup / arraylist-append over the same sizes + init as the Cajeta side
# (the Ankerl insert+find shape for the hashmaps). Asserts competitors/
# collection.sh emits schema-conformant rows for the present languages (rust std+
# ahash, cpp std+ankerl::unordered_dense, go map/slice, python dict/set/list),
# that every measured row passes its exact closed-form cross-check, and that the
# fast-hashmap competitors (ahash, ankerl) are present. No dataset needed.
set -uo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
PROJ="$( cd -- "${SCRIPT_DIR}/.." &> /dev/null && pwd )"
RUNNER="${PROJ}/competitors/collection.sh"
NCOLS="$(awk -F, '{print NF; exit}' "${PROJ}/competitors/columns.txt")"

fail() { echo "COLL-COMP FAIL: $1" >&2; exit 1; }

echo "[coll-comp] building + running collection competitors (compiles cargo/g++/go on first run)…"
OUT="$(PROFILE_RUN_ID=coll-comp PROFILE_RUN_TS=0 PROFILE_TRIALS=3 bash "$RUNNER" 2>/tmp/coll-comp.err)" \
    || fail "collection.sh exited non-zero"
[[ -n "$OUT" ]] || fail "collection.sh produced no rows"

# (1) every row schema-conformant; benchmark in the expected set; area=collection.
while IFS= read -r row; do
    [[ -z "$row" ]] && continue
    N="$(printf '%s' "$row" | awk -F, '{print NF}')"
    [[ "$N" == "$NCOLS" ]] || fail "row has $N cols, expected $NCOLS: $row"
    bench="$(printf '%s' "$row" | awk -F, '{print $4}')"; area="$(printf '%s' "$row" | awk -F, '{print $5}')"
    case "$bench" in hashmap-int|hashmap-string|hashset-dedup|arraylist-append) ;; *) fail "unexpected benchmark: $bench" ;; esac
    [[ "$area" == "collection" ]] || fail "unexpected area: $area"
    status="$(printf '%s' "$row" | awk -F, '{print $28}')"
    if [[ "$status" == "ok" ]]; then
        min="$(printf '%s' "$row" | awk -F, '{print $17}')"
        chk="$(printf '%s' "$row" | awk -F, '{print $29}')"
        [[ "$min" =~ ^[0-9]+$ && "$min" -gt 0 ]] || fail "ok row min_ns not positive: $row"
        [[ "$chk" == "true" ]]                   || fail "ok row failed closed-form cross-check: $row"
    elif [[ "$status" == "skipped" ]]; then
        note="$(printf '%s' "$row" | awk -F, '{print $30}')"
        [[ -n "$note" ]] || fail "skip row missing reason: $row"
    else
        fail "unexpected status '$status': $row"
    fi
done <<< "$OUT"

# (2) rust + python produced ok rows for all four benchmarks (always present).
for lang in rust python; do
    for b in hashmap-int hashmap-string hashset-dedup arraylist-append; do
        echo "$OUT" | awk -F, -v l="$lang" -v b="$b" '$10==l && $4==b && $28=="ok" && $29=="true"' | grep -q . \
            || fail "no ok $lang $b row"
    done
done

# (3) the fast-hashmap competitor (ahash on rust) is present and ok for hashmap-int.
echo "$OUT" | awk -F, '$10=="rust" && $12=="ahash" && $4=="hashmap-int" && $28=="ok"' | grep -q . \
    || fail "ahash hashmap competitor missing"

# (4) report.py ingests the collection competitor rows.
TMP="$(mktemp -d)"
{ cat "${PROJ}/competitors/columns.txt"; echo "$OUT"; } > "$TMP/results.csv"
echo "schema_version,key,value" > "$TMP/env.csv"
python3 "${PROJ}/report/report.py" "$TMP" >/dev/null 2>&1 || fail "report.py failed on collection rows"
grep -q "hashmap-int" "$TMP/site/index.html" || fail "report site missing hashmap-int rows"
rm -rf "$TMP"

NLANGS="$(echo "$OUT" | awk -F, '$28=="ok"{print $10}' | sort -u | tr '\n' ' ')"
echo "[coll-comp] measured langs: [$NLANGS]; ahash/ankerl fast-hashmaps included"
echo "COLL-COMP OK"
