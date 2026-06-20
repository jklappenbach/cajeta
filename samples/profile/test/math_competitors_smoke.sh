#!/usr/bin/env bash
# Phase B math smoke test — cross-language saxpy/dot-product/matmul over the same
# f64 sizes + init as the Cajeta side. Asserts competitors/math.sh emits schema-
# conformant rows for the present languages (rust/cpp/go scalar, python numpy),
# that every measured row passes its exact-checksum cross-check (148250000.0 /
# 148499899 / 1980000), and that the numpy (BLAS) competitor is present — the
# headline that contrasts a tuned library against the scalar kernels. No dataset.
set -uo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
PROJ="$( cd -- "${SCRIPT_DIR}/.." &> /dev/null && pwd )"
RUNNER="${PROJ}/competitors/math.sh"
NCOLS="$(awk -F, '{print NF; exit}' "${PROJ}/competitors/columns.txt")"

fail() { echo "MATH-COMP FAIL: $1" >&2; exit 1; }

echo "[math-comp] building + running math competitors (compiles cargo/g++/go on first run)…"
OUT="$(PROFILE_RUN_ID=math-comp PROFILE_RUN_TS=0 PROFILE_TRIALS=3 bash "$RUNNER" 2>/tmp/math-comp.err)" \
    || fail "math.sh exited non-zero"
[[ -n "$OUT" ]] || fail "math.sh produced no rows"

# (1) every row schema-conformant; benchmark in the expected set; area=math.
while IFS= read -r row; do
    [[ -z "$row" ]] && continue
    N="$(printf '%s' "$row" | awk -F, '{print NF}')"
    [[ "$N" == "$NCOLS" ]] || fail "row has $N cols, expected $NCOLS: $row"
    bench="$(printf '%s' "$row" | awk -F, '{print $4}')"; area="$(printf '%s' "$row" | awk -F, '{print $5}')"
    case "$bench" in saxpy|dot-product|matmul) ;; *) fail "unexpected benchmark: $bench" ;; esac
    [[ "$area" == "math" ]] || fail "unexpected area: $area"
    status="$(printf '%s' "$row" | awk -F, '{print $28}')"
    if [[ "$status" == "ok" ]]; then
        min="$(printf '%s' "$row" | awk -F, '{print $17}')"
        chk="$(printf '%s' "$row" | awk -F, '{print $29}')"
        [[ "$min" =~ ^[0-9]+$ && "$min" -gt 0 ]] || fail "ok row min_ns not positive: $row"
        [[ "$chk" == "true" ]]                   || fail "ok row failed exact-checksum cross-check: $row"
    elif [[ "$status" == "skipped" ]]; then
        note="$(printf '%s' "$row" | awk -F, '{print $30}')"
        [[ -n "$note" ]] || fail "skip row missing reason: $row"
    else
        fail "unexpected status '$status': $row"
    fi
done <<< "$OUT"

# (2) rust + go produced ok rows for all three kernels (always present).
for lang in rust go; do
    for b in saxpy dot-product matmul; do
        echo "$OUT" | awk -F, -v l="$lang" -v b="$b" '$10==l && $4==b && $28=="ok" && $29=="true"' | grep -q . \
            || fail "no ok $lang $b row"
    done
done

# (3) the numpy (BLAS) competitor is present and ok for matmul.
echo "$OUT" | awk -F, '$10=="python" && $12=="numpy" && $4=="matmul" && $28=="ok"' | grep -q . \
    || fail "numpy matmul (BLAS) competitor missing"

# (4) report.py ingests the math competitor rows.
TMP="$(mktemp -d)"
{ cat "${PROJ}/competitors/columns.txt"; echo "$OUT"; } > "$TMP/results.csv"
echo "schema_version,key,value" > "$TMP/env.csv"
python3 "${PROJ}/report/report.py" "$TMP" >/dev/null 2>&1 || fail "report.py failed on math rows"
grep -q "matmul" "$TMP/site/index.html" || fail "report site missing matmul rows"
rm -rf "$TMP"

NLANGS="$(echo "$OUT" | awk -F, '$28=="ok"{print $10}' | sort -u | tr '\n' ' ')"
echo "[math-comp] measured langs: [$NLANGS]; numpy BLAS headline included"
echo "MATH-COMP OK"
