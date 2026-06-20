#!/usr/bin/env bash
# Phase B string smoke test — cross-language string competitors over the same
# ~360 KB ASCII text the Cajeta side uses. Asserts competitors/string.sh emits
# schema-conformant string-search/string-replace/string-uppercase/string-build-
# concat rows for the present languages (rust stdlib + memchr, cpp std::string,
# go strings, python str), that every measured row passes its cross-check, and
# that the SIMD search competitor (memchr) is present. No dataset needed.
set -uo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
PROJ="$( cd -- "${SCRIPT_DIR}/.." &> /dev/null && pwd )"
RUNNER="${PROJ}/competitors/string.sh"
NCOLS="$(awk -F, '{print NF; exit}' "${PROJ}/competitors/columns.txt")"

fail() { echo "STRING-COMP FAIL: $1" >&2; exit 1; }

echo "[string-comp] building + running string competitors (compiles cargo/g++/go on first run)…"
OUT="$(PROFILE_RUN_ID=string-comp PROFILE_RUN_TS=0 PROFILE_TRIALS=3 bash "$RUNNER" 2>/tmp/string-comp.err)" \
    || fail "string.sh exited non-zero"
[[ -n "$OUT" ]] || fail "string.sh produced no rows"

# (1) every row schema-conformant; benchmark in the expected set; area=string.
while IFS= read -r row; do
    [[ -z "$row" ]] && continue
    N="$(printf '%s' "$row" | awk -F, '{print NF}')"
    [[ "$N" == "$NCOLS" ]] || fail "row has $N cols, expected $NCOLS: $row"
    bench="$(printf '%s' "$row" | awk -F, '{print $4}')"; area="$(printf '%s' "$row" | awk -F, '{print $5}')"
    case "$bench" in string-search|string-replace|string-uppercase|string-build-concat) ;; *) fail "unexpected benchmark: $bench" ;; esac
    [[ "$area" == "string" ]] || fail "unexpected area: $area"
    status="$(printf '%s' "$row" | awk -F, '{print $28}')"
    if [[ "$status" == "ok" ]]; then
        min="$(printf '%s' "$row" | awk -F, '{print $17}')"
        chk="$(printf '%s' "$row" | awk -F, '{print $29}')"
        [[ "$min" =~ ^[0-9]+$ && "$min" -gt 0 ]] || fail "ok row min_ns not positive: $row"
        [[ "$chk" == "true" ]]                   || fail "ok row failed cross-check: $row"
    elif [[ "$status" == "skipped" ]]; then
        note="$(printf '%s' "$row" | awk -F, '{print $30}')"
        [[ -n "$note" ]] || fail "skip row missing reason: $row"
    else
        fail "unexpected status '$status': $row"
    fi
done <<< "$OUT"

# (2) the always-present languages produced ok rows for all four benchmarks.
for lang in rust python; do
    for b in string-search string-replace string-uppercase string-build-concat; do
        echo "$OUT" | awk -F, -v l="$lang" -v b="$b" '$10==l && $4==b && $28=="ok" && $29=="true"' | grep -q . \
            || fail "no ok $lang $b row"
    done
done

# (3) the SIMD search competitor (memchr) is present and ok.
echo "$OUT" | awk -F, '$4=="string-search" && $12=="memchr" && $28=="ok"' | grep -q . \
    || fail "memchr search competitor missing"

# (4) report.py ingests the string competitor rows.
TMP="$(mktemp -d)"
{ cat "${PROJ}/competitors/columns.txt"; echo "$OUT"; } > "$TMP/results.csv"
echo "schema_version,key,value" > "$TMP/env.csv"
python3 "${PROJ}/report/report.py" "$TMP" >/dev/null 2>&1 || fail "report.py failed on string rows"
grep -q "string-search" "$TMP/site/index.html" || fail "report site missing string rows"
rm -rf "$TMP"

NLANGS="$(echo "$OUT" | awk -F, '$28=="ok"{print $10}' | sort -u | tr '\n' ' ')"
echo "[string-comp] measured langs: [$NLANGS]; memchr SIMD search included"
echo "STRING-COMP OK"
