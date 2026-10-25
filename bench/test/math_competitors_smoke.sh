#!/usr/bin/env bash
# Phase B math smoke test — cross-language saxpy/dot-product/matmul over the same
# f64 sizes + init as the Cajeta side. Asserts competitors/math.sh emits schema-
# conformant rows for the present languages (rust/cpp/go scalar, python numpy),
# that every measured row passes its exact-checksum cross-check (148250000.0 /
# 148499899 / 1980000), and that the numpy (BLAS) competitor is present — the
# headline that contrasts a tuned library against the scalar kernels. No dataset.
#
# Part B (benchmark-reference unit 11, plan 11.1.2): drivers/math/numeric.sh
# under CAJETA_BENCH_SMOKE=1 — every math/ page id each language's page row
# names (NumPy/SciPy, Eigen/FFTW/pocketfft, ndarray/nalgebra/rustfft/...,
# gonum, EJML/JTransforms/...) emits rows through run.sh with status ok and
# check_ok true, with both variants of the linalg ids and of fft-1d.
# SKIP_NUMERIC=1 runs part A only.
set -uo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
source "$SCRIPT_DIR/csv_lib.sh"   # cawk / csv_fields: CSV-aware column checks
PROJ="$( cd -- "${SCRIPT_DIR}/.." &> /dev/null && pwd )"
RUNNER="${PROJ}/competitors/drivers/math/math.sh"   # spans lang/ (saxpy, dot-product) and math/ (matmul)
source "${PROJ}/competitors/lib/rows.sh"
[[ -d "$HOME/.cargo/bin" ]] && export PATH="$HOME/.cargo/bin:$PATH"   # rustup is user-level
NCOLS="$(cawk '{print NF; exit}' "${PROJ}/competitors/columns.txt")"

fail() { echo "MATH-COMP FAIL: $1" >&2; exit 1; }

echo "[math-comp] building + running math competitors (compiles cargo/g++/go on first run)…"
OUT="$(PROFILE_RUN_ID=math-comp PROFILE_RUN_TS=0 PROFILE_TRIALS=3 bash "$RUNNER" 2>"${CAJETA_BENCH_OUT:-/tmp}/math-comp.err" | rows_v2)" \
    || fail "math.sh exited non-zero"
[[ -n "$OUT" ]] || fail "math.sh produced no rows"

# (1) every row schema-conformant; benchmark in the expected set; area=math.
while IFS= read -r row; do
    [[ -z "$row" ]] && continue
    N="$(printf '%s' "$row" | cawk '{print NF}')"
    [[ "$N" == "$NCOLS" ]] || fail "row has $N cols, expected $NCOLS: $row"
    [[ "$(printf '%s' "$row" | cawk '{print $1}')" == "2" ]] || fail "row is not schema_version 2: $row"
    printf '%s' "$row" | cawk '$33=="" || $34=="" || $37=="" || $37!=$10 {exit 1}' \
        || fail "row lacks machine/os/platform (platform must be the language): $row"
    bench="$(printf '%s' "$row" | cawk '{print $4}')"; area="$(printf '%s' "$row" | cawk '{print $5}')"
    case "$bench" in saxpy|dot-product|matmul) ;; *) fail "unexpected benchmark: $bench" ;; esac
    case "$bench" in matmul) want=math ;; *) want=lang ;; esac
    [[ "$area" == "$want" ]] || fail "unexpected area for $bench: $area (want $want)"
    status="$(printf '%s' "$row" | cawk '{print $28}')"
    if [[ "$status" == "ok" ]]; then
        min="$(printf '%s' "$row" | cawk '{print $17}')"
        chk="$(printf '%s' "$row" | cawk '{print $29}')"
        [[ "$min" =~ ^[0-9]+$ && "$min" -gt 0 ]] || fail "ok row min_ns not positive: $row"
        [[ "$chk" == "true" ]]                   || fail "ok row failed exact-checksum cross-check: $row"
    elif [[ "$status" == "skipped" ]]; then
        note="$(printf '%s' "$row" | cawk '{print $30}')"
        [[ -n "$note" ]] || fail "skip row missing reason: $row"
    else
        fail "unexpected status '$status': $row"
    fi
done < <(csv_fields <<<"$OUT")

# (2) rust + go produced ok rows for all three kernels (always present).
for lang in rust go; do
    for b in saxpy dot-product matmul; do
        echo "$OUT" | cawk -v l="$lang" -v b="$b" '$10==l && $4==b && $28=="ok" && $29=="true"' | grep -q . \
            || fail "no ok $lang $b row"
    done
done

# (2b) each package keeps only its own rows: math.sh = matmul, lang.sh = saxpy + dot-product.
[[ "$(printf '%s\n' "$OUT" | rows_v2 math | cawk '{print $4}' | sort -u | tr '\n' ' ')" == "matmul " ]] || fail "math filter kept non-matmul rows"
[[ "$(printf '%s\n' "$OUT" | rows_v2 lang | cawk '{print $4}' | sort -u | tr '\n' ' ')" == "dot-product saxpy " ]] || fail "lang filter rows wrong"

# (3) the numpy (BLAS) competitor is present and ok for matmul.
echo "$OUT" | cawk '$10=="python" && $12=="numpy" && $4=="matmul" && $28=="ok"' | grep -q . \
    || fail "numpy matmul (BLAS) competitor missing"


# ---- part B: the numeric programs (unit 11) ----
if [[ "${SKIP_NUMERIC:-0}" != 1 ]]; then
    echo "[math-comp] numeric: building + running python cpp rust go java (smoke sizes)…"
    t0=$SECONDS
    NOUT="$(cd "${PROJ}/competitors" && CAJETA_BENCH_SMOKE=1 PROFILE_RUN_ID=math-comp PROFILE_RUN_TS=0 PROFILE_TRIALS=3 PROFILE_WARMUP=1 \
            bash run.sh math numeric 2>"${CAJETA_BENCH_OUT:-/tmp}/math-comp-numeric.err")" || fail "run.sh math numeric exited non-zero (see ${CAJETA_BENCH_OUT:-/tmp}/math-comp-numeric.err)"
    echo "[math-comp] numeric run $((SECONDS - t0)) s"
    [[ -n "$NOUT" ]] || fail "numeric produced no rows"
    while IFS= read -r row; do
        [[ -z "$row" ]] && continue
        IFS="$CSV_FS" read -r -a c <<< "$row"
        [[ "${#c[@]}" -ge 36 && "${c[0]}" == "2" && "${c[4]}" == "math" ]] || fail "numeric row not v2 math: $row"
        [[ "${c[27]}" == "ok" && "${c[28]}" == "true" ]] || fail "numeric ${c[9]} ${c[11]} ${c[3]} [${c[30]}] status=${c[27]} check=${c[28]} notes=${c[29]}"
        [[ "${c[16]}" =~ ^[0-9]+$ && "${c[16]}" -gt 0 ]] || fail "numeric ${c[9]} ${c[3]} min_ns=${c[16]}"
    done < <(csv_fields <<<"$NOUT")
    T="tensor-elementwise tensor-broadcast tensor-reduce tensor-transpose tensor-cast"
    L="linalg-solve linalg-inverse linalg-qr linalg-svd"
    G="ray-sphere ray-aabb rotation-compose transform-apply"
    want_ids() {
        case "$1" in
            python) echo "tensor-element-get tensor-element-set $T $L fft-1d fft-2d random-uniform random-normal npy-load-i32 npy-save-i32 poly-eval poly-fit stats-moments stats-histogram transform-apply" ;;
            cpp)    echo "tensor-element-get tensor-element-set $T $L fft-1d fft-2d random-uniform random-normal poly-eval poly-fit $G" ;;
            rust)   echo "tensor-element-get tensor-element-set $T $L fft-1d fft-2d random-uniform random-normal npy-load-i32 npy-save-i32 poly-eval poly-fit stats-moments stats-histogram $G" ;;
            go)     echo "$T $L fft-1d fft-2d random-uniform random-normal poly-eval poly-fit stats-moments stats-histogram $G" ;;
            java)   echo "$T $L fft-1d fft-2d random-uniform random-normal npy-load-i32 npy-save-i32 poly-eval stats-moments stats-histogram $G" ;;
        esac
    }
    for lang in python cpp rust go java; do
        for b in $(want_ids "$lang"); do
            vs=""
            case "$b" in linalg-*) vs="512 2048" ;; fft-1d) vs="2^16 2^20" ;; esac
            if [[ -z "$vs" ]]; then
                echo "$NOUT" | cawk -v l="$lang" -v b="$b" '$10==l && $4==b' | grep -q . || fail "no $lang math/$b row"
            else
                for v in $vs; do
                    echo "$NOUT" | cawk -v l="$lang" -v b="$b" -v v="$v" '$10==l && $4==b && $31==v' | grep -q . || fail "no $lang math/$b [$v] row"
                done
            fi
        done
    done
    # the fft libraries the page names: FFTW and pocketfft both on the C++ side
    for lib in fftw pocketfft; do
        echo "$NOUT" | cawk -v l="$lib" '$10=="cpp" && $12==l && $4=="fft-1d"' | grep -q . || fail "no cpp $lib fft-1d row"
    done
    echo "[math-comp] numeric: $(echo "$NOUT" | grep -c .) rows ok across $(echo "$NOUT" | cawk '{print $10}' | sort -u | tr '\n' ' ')"
fi

NLANGS="$(echo "$OUT" | cawk '$28=="ok"{print $10}' | sort -u | tr '\n' ' ')"
echo "[math-comp] measured langs: [$NLANGS]; numpy BLAS headline included"
echo "MATH-COMP OK"
