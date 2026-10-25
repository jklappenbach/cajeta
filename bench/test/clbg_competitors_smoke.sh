#!/usr/bin/env bash
# Phase B CLBG smoke test — cross-language Computer Language Benchmarks Game
# classics (mandelbrot 800² / fannkuch-redux N=10 / spectral-norm N=100) at the
# same sizes as the Cajeta side. Asserts competitors/lang.sh clbg emits schema-
# conformant rows for the present languages (rust/cpp/go scalar, python numpy+
# CPython), and that every measured row passes its canonical-checksum cross-check
# (in-set 254099 / 38+73196 / ≈1.274224). Uses a tiny warmup/trials because pure-
# CPython fannkuch(10) is ~4s/run (intentionally — the interpreter-cost data).
set -uo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
source "$SCRIPT_DIR/csv_lib.sh"   # cawk / csv_fields: CSV-aware column checks
PROJ="$( cd -- "${SCRIPT_DIR}/.." &> /dev/null && pwd )"
RUNNER="${PROJ}/competitors/lang.sh"   # program clbg of package lang
NCOLS="$(cawk '{print NF; exit}' "${PROJ}/competitors/columns.txt")"

fail() { echo "CLBG-COMP FAIL: $1" >&2; exit 1; }

echo "[clbg-comp] building + running clbg competitors (compiles cargo/g++/go; pure-CPython fannkuch is slow)…"
OUT="$(PROFILE_RUN_ID=clbg-comp PROFILE_RUN_TS=0 PROFILE_WARMUP=1 PROFILE_TRIALS=2 bash "$RUNNER" clbg 2>/tmp/clbg-comp.err)" \
    || fail "clbg.sh exited non-zero"
[[ -n "$OUT" ]] || fail "clbg.sh produced no rows"

# (1) every row schema-conformant; benchmark in the expected set; area=clbg.
while IFS= read -r row; do
    [[ -z "$row" ]] && continue
    N="$(printf '%s' "$row" | cawk '{print NF}')"
    [[ "$N" == "$NCOLS" ]] || fail "row has $N cols, expected $NCOLS: $row"
    [[ "$(printf '%s' "$row" | cawk '{print $1}')" == "2" ]] || fail "row is not schema_version 2: $row"
    printf '%s' "$row" | cawk '$33=="" || $34=="" || $37=="" || $37!=$10 {exit 1}' \
        || fail "row lacks machine/os/platform (platform must be the language): $row"
    bench="$(printf '%s' "$row" | cawk '{print $4}')"; area="$(printf '%s' "$row" | cawk '{print $5}')"
    case "$bench" in clbg-mandelbrot|clbg-fannkuch-redux|clbg-spectral-norm) ;; *) fail "unexpected benchmark: $bench" ;; esac
    [[ "$area" == "lang" ]] || fail "unexpected area: $area"
    status="$(printf '%s' "$row" | cawk '{print $28}')"
    if [[ "$status" == "ok" ]]; then
        min="$(printf '%s' "$row" | cawk '{print $17}')"
        chk="$(printf '%s' "$row" | cawk '{print $29}')"
        [[ "$min" =~ ^[0-9]+$ && "$min" -gt 0 ]] || fail "ok row min_ns not positive: $row"
        [[ "$chk" == "true" ]]                   || fail "ok row failed canonical-checksum cross-check: $row"
    elif [[ "$status" == "skipped" ]]; then
        note="$(printf '%s' "$row" | cawk '{print $30}')"
        [[ -n "$note" ]] || fail "skip row missing reason: $row"
    else
        fail "unexpected status '$status': $row"
    fi
done < <(csv_fields <<<"$OUT")

# (2) rust + go produced ok rows for all three classics (always present, verified checksums).
for lang in rust go; do
    for b in clbg-mandelbrot clbg-fannkuch-redux clbg-spectral-norm; do
        echo "$OUT" | cawk -v l="$lang" -v b="$b" '$10==l && $4==b && $28=="ok" && $29=="true"' | grep -q . \
            || fail "no ok $lang $b row"
    done
done


NLANGS="$(echo "$OUT" | cawk '$28=="ok"{print $10}' | sort -u | tr '\n' ' ')"
echo "[clbg-comp] measured langs: [$NLANGS]; canonical checksums verified"
echo "CLBG-COMP OK"
