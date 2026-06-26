#!/usr/bin/env bash
# gpu-matmul-profiling Unit 1 TDD smoke test (Cajeta side). Asserts the GPU
# @Kernel matmul bench builds once for multiple backends, runs per-backend via
# CAJETA_XPU_BACKEND, and emits backend-tagged, size-swept rows with check_ok.
#
# Requires a working GPU (gfx1151 HIP + RADV Vulkan on this box). On a host with
# no GPU the run-asserts degrade to the skip-clean check (1.1.c) only.
set -euo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
PROJ="$( cd -- "${SCRIPT_DIR}/.." &> /dev/null && pwd )"
REPO_ROOT="$( cd -- "${PROJ}/../.." &> /dev/null && pwd )"
CAJETA="${CAJETA:-${REPO_ROOT}/build-cajeta/src/cajeta}"

# CSV columns (1-based): 4=benchmark 5=area 9=input_size 28=status 29=check_ok
# 31=variant 32=backend  (backend is appended last to keep prior indices stable)
COL_BENCH=4; COL_AREA=5; COL_STATUS=28; COL_CHECK=29; COL_VARIANT=31; COL_BACKEND=32

fail() { echo "GPU FAIL: $1" >&2; exit 1; }
[[ -x "$CAJETA" ]] || fail "cajeta not found at $CAJETA"

cd "$PROJ"
echo "[gpu] building gpu flavor (amdgpu,cpu / gfx1151)..."
"$CAJETA" gpu >/tmp/gpu-build.log 2>&1 || { tail -25 /tmp/gpu-build.log; fail "gpu build failed"; }
EXE="$PROJ/build/profile-gpu"
[[ -x "$EXE" ]] || EXE="$PROJ/build/profile"
[[ -x "$EXE" ]] || fail "gpu exe not found"

# 1.1.b — per-backend run-asserts: each selected backend yields gpu/matmul rows
# with status=ok, check_ok=true, and backend == the selected backend.
run_backend() {
    local bk="$1" env_bk="$2"
    local out="/tmp/profile-gpu-${bk}.csv"
    rm -f "$out"
    echo "[gpu] running CAJETA_XPU_BACKEND=$env_bk ..."
    if ! CAJETA_XPU_BACKEND="$env_bk" PROFILE_RUN_ID="gpu-$bk" \
            "$EXE" --run --area gpu --out "$out" >/tmp/gpu-run-$bk.log 2>&1; then
        grep -vE "example:" /tmp/gpu-run-$bk.log | tail -15
        fail "--run crashed for backend $env_bk"
    fi
    local rows
    rows="$(awk -F, -v b=matmul -v c="$COL_BENCH" 'NR>1 && $c==b {print}' "$out")"
    [[ -n "$rows" ]] || fail "no matmul rows for backend $bk"
    local n_ok=0
    while IFS= read -r row; do
        [[ -n "$row" ]] || continue
        local st ck va be
        st="$(printf '%s' "$row" | awk -F, -v c="$COL_STATUS"  '{print $c}')"
        ck="$(printf '%s' "$row" | awk -F, -v c="$COL_CHECK"   '{print $c}')"
        va="$(printf '%s' "$row" | awk -F, -v c="$COL_VARIANT" '{print $c}')"
        be="$(printf '%s' "$row" | awk -F, -v c="$COL_BACKEND" '{print $c}')"
        [[ "$st" == "ok" ]]    || fail "matmul[$va] backend=$bk status=$st"
        [[ "$ck" == "true" ]]  || fail "matmul[$va] backend=$bk check_ok=$ck"
        [[ "$be" == "$bk" ]]   || fail "matmul[$va] backend col='$be' != '$bk'"
        echo "  [gpu] ok matmul variant=$va backend=$be"
        n_ok=$((n_ok+1))
    done <<< "$rows"
    [[ "$n_ok" -ge 2 ]] || fail "expected >=2 swept sizes for backend $bk, got $n_ok"
}

if [[ "${GPU_SMOKE_SKIP_DEVICE:-}" != "1" ]]; then
    run_backend hip hip
    # Vulkan deferred — gpu-vulkan-f64 plan (8-wide SPIR-V limit + 4-wide check=false).
else
    echo "[gpu] GPU_SMOKE_SKIP_DEVICE=1 — skipping on-device run-asserts"
fi

# 1.1.c — skip-clean: an unavailable backend yields skipped rows, exit 0 (no crash).
echo "[gpu] skip-clean check (bogus backend)..."
OUT_SKIP="/tmp/profile-gpu-skip.csv"; rm -f "$OUT_SKIP"
CAJETA_XPU_BACKEND="nonesuch" PROFILE_RUN_ID="gpu-skip" \
    "$EXE" --run --area gpu --out "$OUT_SKIP" >/tmp/gpu-run-skip.log 2>&1 \
    || fail "suite crashed (should skip cleanly) for unavailable backend"
skiprow="$(awk -F, -v b=matmul -v c="$COL_BENCH" 'NR>1 && $c==b {print; exit}' "$OUT_SKIP")"
[[ -n "$skiprow" ]] || fail "no matmul row at all for unavailable backend"
skst="$(printf '%s' "$skiprow" | awk -F, -v c="$COL_STATUS" '{print $c}')"
[[ "$skst" == "skipped" || "$skst" == "ok" ]] || fail "unavailable-backend status=$skst (want skipped)"
echo "[gpu] skip-clean ok (status=$skst, exit 0)"

echo "GPU OK"
