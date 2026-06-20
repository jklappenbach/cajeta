#!/usr/bin/env bash
# Phase B CLBG smoke test — cross-language Computer Language Benchmarks Game
# classics (mandelbrot 800² / fannkuch-redux N=10 / spectral-norm N=100) at the
# same sizes as the Cajeta side. Asserts competitors/clbg.sh emits schema-
# conformant rows for the present languages (rust/cpp/go scalar, python numpy+
# CPython), and that every measured row passes its canonical-checksum cross-check
# (in-set 254099 / 38+73196 / ≈1.274224). Uses a tiny warmup/trials because pure-
# CPython fannkuch(10) is ~4s/run (intentionally — the interpreter-cost data).
set -uo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
PROJ="$( cd -- "${SCRIPT_DIR}/.." &> /dev/null && pwd )"
RUNNER="${PROJ}/competitors/clbg.sh"
NCOLS="$(awk -F, '{print NF; exit}' "${PROJ}/competitors/columns.txt")"

fail() { echo "CLBG-COMP FAIL: $1" >&2; exit 1; }

echo "[clbg-comp] building + running clbg competitors (compiles cargo/g++/go; pure-CPython fannkuch is slow)…"
OUT="$(PROFILE_RUN_ID=clbg-comp PROFILE_RUN_TS=0 PROFILE_WARMUP=1 PROFILE_TRIALS=2 bash "$RUNNER" 2>/tmp/clbg-comp.err)" \
    || fail "clbg.sh exited non-zero"
[[ -n "$OUT" ]] || fail "clbg.sh produced no rows"

# (1) every row schema-conformant; benchmark in the expected set; area=clbg.
while IFS= read -r row; do
    [[ -z "$row" ]] && continue
    N="$(printf '%s' "$row" | awk -F, '{print NF}')"
    [[ "$N" == "$NCOLS" ]] || fail "row has $N cols, expected $NCOLS: $row"
    bench="$(printf '%s' "$row" | awk -F, '{print $4}')"; area="$(printf '%s' "$row" | awk -F, '{print $5}')"
    case "$bench" in clbg-mandelbrot|clbg-fannkuch-redux|clbg-spectral-norm) ;; *) fail "unexpected benchmark: $bench" ;; esac
    [[ "$area" == "clbg" ]] || fail "unexpected area: $area"
    status="$(printf '%s' "$row" | awk -F, '{print $28}')"
    if [[ "$status" == "ok" ]]; then
        min="$(printf '%s' "$row" | awk -F, '{print $17}')"
        chk="$(printf '%s' "$row" | awk -F, '{print $29}')"
        [[ "$min" =~ ^[0-9]+$ && "$min" -gt 0 ]] || fail "ok row min_ns not positive: $row"
        [[ "$chk" == "true" ]]                   || fail "ok row failed canonical-checksum cross-check: $row"
    elif [[ "$status" == "skipped" ]]; then
        note="$(printf '%s' "$row" | awk -F, '{print $30}')"
        [[ -n "$note" ]] || fail "skip row missing reason: $row"
    else
        fail "unexpected status '$status': $row"
    fi
done <<< "$OUT"

# (2) rust + go produced ok rows for all three classics (always present, verified checksums).
for lang in rust go; do
    for b in clbg-mandelbrot clbg-fannkuch-redux clbg-spectral-norm; do
        echo "$OUT" | awk -F, -v l="$lang" -v b="$b" '$10==l && $4==b && $28=="ok" && $29=="true"' | grep -q . \
            || fail "no ok $lang $b row"
    done
done

# (3) report.py ingests the clbg competitor rows.
TMP="$(mktemp -d)"
{ cat "${PROJ}/competitors/columns.txt"; echo "$OUT"; } > "$TMP/results.csv"
echo "schema_version,key,value" > "$TMP/env.csv"
python3 "${PROJ}/report/report.py" "$TMP" >/dev/null 2>&1 || fail "report.py failed on clbg rows"
grep -q "clbg-mandelbrot" "$TMP/site/index.html" || fail "report site missing clbg rows"
rm -rf "$TMP"

NLANGS="$(echo "$OUT" | awk -F, '$28=="ok"{print $10}' | sort -u | tr '\n' ' ')"
echo "[clbg-comp] measured langs: [$NLANGS]; canonical checksums verified"
echo "CLBG-COMP OK"
