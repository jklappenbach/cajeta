#!/usr/bin/env bash
# xpu competitor smoke (gpu-matmul-profiling Unit 4, moved to bench/; xpu
# package + schema v2 since benchmark-reference). By default it is BUILD-ONLY
# and never touches a GPU:
#  (1) `competitors/xpu.sh gpu` under PROFILE_BUILD_ONLY=1 compiles every
#      program it can (python py_compile; cpp with hipcc when present) and
#      emits only schema-v2 `skipped` rows for xpu/gemm-f32 with a reason;
#      the python program must compile;
#  (2) skip-clean: an absent GPU venv yields skipped rows, exit 0.
# CAJETA_BENCH_DEVICE=1 adds the on-device leg (PyTorch rows ok, check_ok,
# backend hip) — run it only on a quiet box with the GPU free.
set -euo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
PROJ="$( cd -- "${SCRIPT_DIR}/.." &> /dev/null && pwd )"
fail() { echo "GPU-COMPETITORS FAIL: $1" >&2; exit 1; }
cd "$PROJ"
NCOLS="$(awk -F, '{print NF; exit}' competitors/columns.txt)"

check_rows() {   # check_rows <csv> <want-status-regex>
    local f="$1" want="$2" r n=0
    while IFS= read -r r; do
        [[ -n "$r" ]] || continue
        [[ "$(printf '%s' "$r" | awk -F, '{print NF}')" == "$NCOLS" ]] || fail "row is not $NCOLS columns: $r"
        printf '%s' "$r" | awk -F, -v w="$want" '$1=="2" && $5=="xpu" && ($4=="gemm-f32" || $4=="gemm-f16") && $28 ~ w && $32=="hip" && $33!="" && $34!="" && $37==$10 {ok=1} END {exit !ok}' \
            || fail "bad xpu row: $r"
        if [[ "$(printf '%s' "$r" | awk -F, '{print $28}')" == "skipped" ]]; then
            [[ -n "$(printf '%s' "$r" | awk -F, '{print $30}')" ]] || fail "skip row without reason: $r"
        fi
        n=$((n + 1))
    done < "$f"
    [[ "$n" -gt 0 ]] || fail "no xpu rows in $f"
    echo "$n"
}

# (1) build-only
OUT=/tmp/cajeta-bench-gpu-comp-build.csv
PROFILE_BUILD_ONLY=1 PROFILE_RUN_ID=gpu-build bash competitors/xpu.sh gpu > "$OUT" 2>/tmp/gpu-comp-build.err \
    || { cat /tmp/gpu-comp-build.err; fail "xpu.sh gpu (build-only) failed"; }
n="$(check_rows "$OUT" '^skipped$')"
grep -q 'python,.*build-only: compiled; not run' "$OUT" || fail "python GPU competitor did not compile"
echo "[gpu-comp] build-only: $n skipped rows, python compiled, cpp: $(awk -F, '$10=="cpp"{print $30; exit}' "$OUT")"

# (2) skip-clean
OUT2=/tmp/cajeta-bench-gpu-comp-skip.csv
PROFILE_LANGS=python PROFILE_GPU_PY=/nonexistent/python PROFILE_RUN_ID=gpu-skip \
    bash competitors/xpu.sh gpu > "$OUT2" 2>/dev/null || fail "skip path crashed (should exit 0)"
check_rows "$OUT2" '^skipped$' >/dev/null
echo "[gpu-comp] skip-clean ok"

# (3) on-device, opt-in only
if [[ "${CAJETA_BENCH_DEVICE:-}" == "1" ]]; then
    OUT3=/tmp/cajeta-bench-gpu-comp-device.csv
    PROFILE_LANGS=python PROFILE_RUN_ID=gpu-comp bash competitors/xpu.sh gpu > "$OUT3" 2>/dev/null || fail "xpu.sh gpu (device) crashed"
    check_rows "$OUT3" '^(ok|skipped)$' >/dev/null
    awk -F, '$28=="ok" && $29!="true"' "$OUT3" | grep -q . && fail "an ok device row failed its check"
    echo "[gpu-comp] device leg: $(awk -F, '$28=="ok"' "$OUT3" | wc -l) ok rows"
else
    echo "[gpu-comp] device leg not run (set CAJETA_BENCH_DEVICE=1 on a quiet box)"
fi

echo "GPU-COMPETITORS OK"
