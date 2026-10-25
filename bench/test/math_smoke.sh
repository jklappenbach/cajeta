#!/usr/bin/env bash
# Unit 11 TDD smoke test (math, Cajeta side). Self-generating. Asserts the numeric
# kernels are ok (checksum / identity-matmul cross-checks) with input_size.
set -euo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
PROJ="$( cd -- "${SCRIPT_DIR}/.." &> /dev/null && pwd )"
REPO_ROOT="$( cd -- "${PROJ}/.." &> /dev/null && pwd )"
CAJETA="${CAJETA:-${REPO_ROOT}/build/src/cajeta}"

fail() { echo "MATH FAIL: $1" >&2; exit 1; }
[[ -x "$CAJETA" ]] || fail "cajeta not found at $CAJETA"

cd "$PROJ"
echo "[math] building (release)..."
"$CAJETA" release >/dev/null || fail "release build failed"

OUT="/tmp/cajeta-bench-math.csv"
OUTL="/tmp/cajeta-bench-math-lang.csv"
rm -f "$OUT" "$OUTL"
CAJETA_BENCH_SMOKE=1 PROFILE_RUN_ID=math-run "$PROJ/build/bench" --run --area math --out "$OUT" >/tmp/math-run.log 2>&1 \
    || { tail -15 /tmp/math-run.log; fail "--run --area math crashed"; }
# saxpy and dot-product moved to lang/ (spec §6.1 Math: Vector<T,N> SIMD).
CAJETA_BENCH_SMOKE=1 PROFILE_RUN_ID=math-run "$PROJ/build/bench" --run --bench saxpy --out "$OUTL" >/tmp/math-run-lang.log 2>&1 \
    || { tail -15 /tmp/math-run-lang.log; fail "--run --bench saxpy crashed"; }
CAJETA_BENCH_SMOKE=1 PROFILE_RUN_ID=math-run "$PROJ/build/bench" --run --bench dot-product --out "$OUTL.2" >>/tmp/math-run-lang.log 2>&1 \
    || { tail -15 /tmp/math-run-lang.log; fail "--run --bench dot-product crashed"; }
tail -n +2 "$OUTL.2" >> "$OUTL"
grep -hE "^  (ok|skipped|invalid|failed)  " /tmp/math-run.log /tmp/math-run-lang.log | sed 's/^/  /' || true

check() {   # check <csv> <benchmark> <want-id area/name> [variant]
    local csv="$1" b="$2" want="$3" v="${4:-}" rows row st ar
    rows="$(awk -F, -v b="$b" -v v="$v" 'NR>1 && $4==b && (v=="" || $31==v)' "$csv")"
    [[ -n "$rows" ]] || fail "no row for $b${v:+/$v}"
    while IFS= read -r row; do
        st="$(printf '%s' "$row" | awk -F, '{print $28}')"
        ar="$(printf '%s' "$row" | awk -F, '{print $5"/"$4}')"
        [[ "$ar" == "$want" ]] || fail "$b id=$ar (want $want)"
        [[ "$st" == "ok" ]] || fail "$b${v:+/$v} status=$st"
        printf '%s' "$row" | awk -F, '{print $30}' | grep -q 'smoke' || fail "$b row not tagged smoke"
        echo "[math] $want${v:+ [$v]} ok  min_ns=$(printf '%s' "$row" | awk -F, '{print $17}')"
    done <<< "$rows"
}
for v in flat-get array host-buffer; do check "$OUT" tensor-element-get math/tensor-element-get "$v"; done
check "$OUT" tensor-element-set math/tensor-element-set
check "$OUT" npy-load-i32 math/npy-load-i32
check "$OUT" matmul math/matmul
check "$OUTL" saxpy lang/saxpy
check "$OUTL" dot-product lang/dot-product
# --area math selects the math package only
others="$(awk -F, 'NR>1 && $5 != "math"' "$OUT" | wc -l)"
[[ "$others" == "0" ]] || fail "--area math emitted $others non-math rows"

echo "MATH OK"
