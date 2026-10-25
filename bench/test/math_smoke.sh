#!/usr/bin/env bash
# math/ smoke (benchmark-reference unit 11, plan 11.1.1): every id on the math
# pages (bench/math/*.md, bench/math/*/*.md) emits a row per variant with
# status ok, check_ok true, a positive min_ns, the catalog id math/<slug> and
# the smoke tag, under CAJETA_BENCH_SMOKE=1 (reduced sizes; each benchmark
# says which in notes). It also still runs lang/saxpy and lang/dot-product,
# the SIMD rows that moved to lang/ (spec §6.1) and were first checked here.
#
# Env: CAJETA (compiler, default <repo>/build/src/cajeta), CAJETA_BENCH_OUT
# (directory for the CSV and logs, default /tmp), NO_BUILD=1 (reuse build/bench).
set -euo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
PROJ="$( cd -- "${SCRIPT_DIR}/.." &> /dev/null && pwd )"
REPO_ROOT="$( cd -- "${PROJ}/.." &> /dev/null && pwd )"
CAJETA="${CAJETA:-${REPO_ROOT}/build/src/cajeta}"
OUTD="${CAJETA_BENCH_OUT:-/tmp}"; mkdir -p "$OUTD"

fail() { echo "MATH FAIL: $1" >&2; exit 1; }
[[ -x "$CAJETA" ]] || fail "cajeta not found at $CAJETA"

cd "$PROJ"
if [[ "${NO_BUILD:-0}" != 1 ]]; then
    echo "[math] building (release)..."
    t0=$SECONDS
    "$CAJETA" release >"$OUTD/math-build.log" 2>&1 || { tail -30 "$OUTD/math-build.log"; fail "release build failed"; }
    echo "[math] build $((SECONDS - t0)) s"
fi

OUT="$OUTD/cajeta-bench-math.csv"
LOG="$OUTD/math-run.log"
rm -f "$OUT"
t0=$SECONDS
CAJETA_BENCH_SMOKE=1 PROFILE_RUN_ID=math-run "$PROJ/build/bench" --run --area math --out "$OUT" >"$LOG" 2>&1 \
    || { tail -30 "$LOG"; fail "--run --area math exited non-zero"; }
echo "[math] run $((SECONDS - t0)) s"
grep -hE "^  (ok|skipped|invalid|failed)  " "$LOG" | sed 's/^/  /' || true

check() {   # check <slug> [variant]
    local b="$1" v="${2:-}" rows row
    rows="$(awk -F, -v b="$b" -v v="$v" 'NR>1 && $4==b && (v=="" || $31==v)' "$OUT")"
    [[ -n "$rows" ]] || fail "no row for math/$b${v:+ [$v]}"
    while IFS= read -r row; do
        IFS=, read -r -a c <<< "$row"
        [[ "${c[4]}/${c[3]}" == "math/$b" ]] || fail "$b id=${c[4]}/${c[3]} (want math/$b)"
        [[ "${c[27]}" == "ok" ]]   || fail "math/$b${v:+ [$v]} status=${c[27]} notes=${c[29]}"
        [[ "${c[28]}" == "true" ]] || fail "math/$b${v:+ [$v]} check_ok=${c[28]}"
        [[ "${c[16]}" =~ ^[0-9]+$ && "${c[16]}" -gt 0 ]] || fail "math/$b${v:+ [$v]} min_ns=${c[16]} (elided?)"
        [[ "${c[29]}" == *smoke* ]] || fail "math/$b row not tagged smoke"
        echo "[math] math/$b${v:+ [$v]} ok  min_ns=${c[16]}"
    done <<< "$rows"
}

for v in flat-get array host-buffer; do check tensor-element-get "$v"; done
check tensor-element-set
for b in tensor-elementwise tensor-broadcast tensor-reduce tensor-transpose tensor-cast matmul; do check "$b"; done
for b in linalg-solve linalg-inverse linalg-qr linalg-svd; do check "$b" 512; check "$b" 2048; done
check fft-1d '2^16'; check fft-1d '2^20'; check fft-2d
for b in random-uniform random-normal npy-load-i32 npy-save-i32 poly-eval poly-fit \
         stats-moments stats-histogram ray-sphere ray-aabb rotation-compose transform-apply; do
    check "$b"
done

# lang/saxpy and lang/dot-product (moved out of math/, kept covered here).
for b in saxpy dot-product; do
    o="$OUTD/cajeta-bench-math-$b.csv"
    CAJETA_BENCH_SMOKE=1 PROFILE_RUN_ID=math-run "$PROJ/build/bench" --run --bench "$b" --out "$o" >>"$LOG" 2>&1 \
        || { tail -15 "$LOG"; fail "--run --bench $b exited non-zero"; }
    awk -F, -v b="$b" 'NR>1 && $4==b && $5=="lang" && $28=="ok" && $29=="true"' "$o" | grep -q . || fail "lang/$b not ok"
    echo "[math] lang/$b ok"
done

# Every id on the pages is covered by the list above (a page row added later
# without a benchmark fails here).
ids="$(grep -ho '^| `math/[a-z0-9-]*`' "$PROJ"/math/*.md "$PROJ"/math/*/*.md | sed 's/.*math\///; s/`//' | sort -u)"
for id in $ids; do
    awk -F, -v b="$id" 'NR>1 && $4==b' "$OUT" | grep -q . || fail "page id math/$id has no row"
done
# --area math selects the math package only
others="$(awk -F, 'NR>1 && $5 != "math"' "$OUT" | wc -l)"
[[ "$others" == "0" ]] || fail "--area math emitted $others non-math rows"

echo "MATH OK ($(echo "$ids" | wc -w) page ids)"
