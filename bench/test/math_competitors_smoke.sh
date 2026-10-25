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
RUNNER="${PROJ}/competitors/drivers/math/math.sh"   # spans lang/ (saxpy, dot-product) and math/ (matmul)
source "${PROJ}/competitors/lib/rows.sh"
NCOLS="$(awk -F, '{print NF; exit}' "${PROJ}/competitors/columns.txt")"

fail() { echo "MATH-COMP FAIL: $1" >&2; exit 1; }

echo "[math-comp] building + running math competitors (compiles cargo/g++/go on first run)…"
OUT="$(PROFILE_RUN_ID=math-comp PROFILE_RUN_TS=0 PROFILE_TRIALS=3 bash "$RUNNER" 2>/tmp/math-comp.err | rows_v2)" \
    || fail "math.sh exited non-zero"
[[ -n "$OUT" ]] || fail "math.sh produced no rows"

# (1) every row schema-conformant; benchmark in the expected set; area=math.
while IFS= read -r row; do
    [[ -z "$row" ]] && continue
    N="$(printf '%s' "$row" | awk -F, '{print NF}')"
    [[ "$N" == "$NCOLS" ]] || fail "row has $N cols, expected $NCOLS: $row"
    [[ "$(printf '%s' "$row" | awk -F, '{print $1}')" == "2" ]] || fail "row is not schema_version 2: $row"
    printf '%s' "$row" | awk -F, '$33=="" || $34=="" || $37=="" || $37!=$10 {exit 1}' \
        || fail "row lacks machine/os/platform (platform must be the language): $row"
    bench="$(printf '%s' "$row" | awk -F, '{print $4}')"; area="$(printf '%s' "$row" | awk -F, '{print $5}')"
    case "$bench" in saxpy|dot-product|matmul) ;; *) fail "unexpected benchmark: $bench" ;; esac
    case "$bench" in matmul) want=math ;; *) want=lang ;; esac
    [[ "$area" == "$want" ]] || fail "unexpected area for $bench: $area (want $want)"
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

# (2b) each package keeps only its own rows: math.sh = matmul, lang.sh = saxpy + dot-product.
[[ "$(printf '%s\n' "$OUT" | rows_v2 math | awk -F, '{print $4}' | sort -u | tr '\n' ' ')" == "matmul " ]] || fail "math filter kept non-matmul rows"
[[ "$(printf '%s\n' "$OUT" | rows_v2 lang | awk -F, '{print $4}' | sort -u | tr '\n' ' ')" == "dot-product saxpy " ]] || fail "lang filter rows wrong"

# (3) the numpy (BLAS) competitor is present and ok for matmul.
echo "$OUT" | awk -F, '$10=="python" && $12=="numpy" && $4=="matmul" && $28=="ok"' | grep -q . \
    || fail "numpy matmul (BLAS) competitor missing"


NLANGS="$(echo "$OUT" | awk -F, '$28=="ok"{print $10}' | sort -u | tr '\n' ' ')"
echo "[math-comp] measured langs: [$NLANGS]; numpy BLAS headline included"
echo "MATH-COMP OK"
