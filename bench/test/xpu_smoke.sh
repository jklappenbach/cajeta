#!/usr/bin/env bash
# xpu/ and gfx/ smoke (benchmark-reference unit 14, plan 14.1.1), per backend.
#
#  (1) build: `cajeta release` (cpu), `cajeta gpu-nvptx` (nvptx sm_89 + cpu, ptxas
#      from CUDA_PATH=/usr/local/cuda) and `cajeta gpu-vulkan` (SPIR-V + cpu);
#      each binary lists every id on bench/xpu/**/*.md and bench/gfx/*.md;
#  (2) cpu: every binary's cpu backend (CAJETA_XPU_BACKEND=cpu) runs every id
#      under CAJETA_BENCH_SMOKE=1 with status ok and check_ok true (reduced
#      sizes where the notes say so); the only skips are the showcase's
#      other-backend variants and mesh-simplify's 1e6 variant, each with its
#      reason; the cross-backend digests agree across the three binaries;
#  (3) the showcase's consistency check fires: a planted wrong digest for
#      another backend turns the cpu variant invalid;
#  (4) cuda (nvptx binary) and vulkan (vulkan binary): with CAJETA_BENCH_NO_GPU=1
#      (the default here: another session owns the GPU) every row is skipped
#      "GPU held by another session (CAJETA_BENCH_NO_GPU=1)"; with
#      CAJETA_BENCH_NO_GPU=0 every row must be ok as in (2) (run that only on a
#      quiet box); hip: skipped "no ROCm on this machine" where there is none;
#  (5) no leg loads a GPU driver library (LD_DEBUG=files: no libcuda, libnvidia,
#      libvulkan, libamdhip64) when the GPU is held, and nvidia-smi / vulkaninfo
#      (which the harness calls to fill the driver column of a device row) are
#      shadowed by stubs that only log the attempt.
#
# Env: CAJETA (compiler, default <repo>/build/src/cajeta), CAJETA_BENCH_OUT
# (directory for CSVs and logs, default /tmp), NO_BUILD=1 (reuse the binaries),
# XPU_BUILDS (default "release gpu-nvptx gpu-vulkan"), CAJETA_BENCH_NO_GPU (default 1).
set -euo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
PROJ="$( cd -- "${SCRIPT_DIR}/.." &> /dev/null && pwd )"
REPO_ROOT="$( cd -- "${PROJ}/.." &> /dev/null && pwd )"
CAJETA="${CAJETA:-${REPO_ROOT}/build/src/cajeta}"
OUTD="${CAJETA_BENCH_OUT:-/tmp}"; mkdir -p "$OUTD"
HELD="${CAJETA_BENCH_NO_GPU:-1}"
BUILDS="${XPU_BUILDS:-release gpu-nvptx gpu-vulkan}"
export CUDA_PATH="${CUDA_PATH:-/usr/local/cuda}"
CHECK="python3 $SCRIPT_DIR/xpu_check.py"

fail() { echo "XPU FAIL: $1" >&2; exit 1; }
[[ -x "$CAJETA" ]] || fail "cajeta not found at $CAJETA"
cd "$PROJ"

bin_of() { case "$1" in release) echo build/bench ;; gpu-nvptx) echo build/bench-nvptx ;; gpu-vulkan) echo build/bench-vulkan ;; esac; }

# (1) build + list
for t in $BUILDS; do
    if [[ "${NO_BUILD:-0}" != 1 ]]; then
        echo "[xpu] cajeta $t ..."
        t0=$SECONDS
        "$CAJETA" "$t" >"$OUTD/xpu-build-$t.log" 2>&1 || { grep -v mma-tiering "$OUTD/xpu-build-$t.log" | tail -30; fail "cajeta $t failed"; }
        echo "[xpu] build $t $((SECONDS - t0)) s"
    fi
    b="$(bin_of "$t")"; [[ -x "$b" ]] || fail "no binary $b for $t"
    "$b" --list > "$OUTD/xpu-list-$t.txt" 2>&1 || fail "$b --list failed"
    $CHECK listed "$PROJ" "$OUTD/xpu-list-$t.txt" | sed "s/^/[xpu] $t: /"
done

# The stubs: the harness runs nvidia-smi / vulkaninfo for a device row's driver
# column; with the GPU held they must not reach the device.
SHIM="$OUTD/xpu-nogpu-shim"; mkdir -p "$SHIM"; : > "$SHIM/attempts.log"
for tool in nvidia-smi vulkaninfo hipconfig rocminfo rocm-smi; do
    printf '#!/bin/sh\necho "%s" >> "%s/attempts.log"\nexit 1\n' "$tool" "$SHIM" > "$SHIM/$tool"; chmod +x "$SHIM/$tool"
done
DIG="$OUTD/xpu-showcase"; rm -rf "$DIG"; mkdir -p "$DIG"

run_leg() {   # run_leg <binary> <backend> <tag> -> CSV path in $LEG_CSV
    local b="$1" be="$2" tag="$3" csv="$OUTD/xpu-$3.csv" log="$OUTD/xpu-$3.log" t0=$SECONDS rc=0
    rm -f "$csv" "$OUTD"/xpu-lddebug-"$tag".*
    if [[ "$HELD" == 1 ]]; then
        env CAJETA_BENCH_NO_GPU=1 CAJETA_XPU_BACKEND="$be" CAJETA_BENCH_SMOKE=1 PROFILE_RUN_ID="xpu-$tag" \
            CAJETA_XPU_SHOWCASE_DIR="$DIG" PATH="$SHIM:$PATH" LD_DEBUG=files LD_DEBUG_OUTPUT="$OUTD/xpu-lddebug-$tag" \
            "$b" --run --area xpu --out "$csv.xpu" >"$log" 2>&1 || rc=$?
        env CAJETA_BENCH_NO_GPU=1 CAJETA_XPU_BACKEND="$be" CAJETA_BENCH_SMOKE=1 PROFILE_RUN_ID="xpu-$tag" \
            CAJETA_XPU_SHOWCASE_DIR="$DIG" PATH="$SHIM:$PATH" LD_DEBUG=files LD_DEBUG_OUTPUT="$OUTD/xpu-lddebug-$tag" \
            "$b" --run --area gfx --out "$csv.gfx" >>"$log" 2>&1 || rc=$?
        if cat "$OUTD"/xpu-lddebug-"$tag".* 2>/dev/null | grep -E "file=[^ ]*(libcuda|libnvidia|libvulkan|libamdhip64|libhsa)" | head -3 | grep -q .; then
            fail "$tag loaded a GPU driver library with the GPU held"
        fi
        rm -f "$OUTD"/xpu-lddebug-"$tag".*
    else
        env CAJETA_XPU_BACKEND="$be" CAJETA_BENCH_SMOKE=1 PROFILE_RUN_ID="xpu-$tag" CAJETA_XPU_SHOWCASE_DIR="$DIG" \
            "$b" --run --area xpu --out "$csv.xpu" >"$log" 2>&1 || rc=$?
        env CAJETA_XPU_BACKEND="$be" CAJETA_BENCH_SMOKE=1 PROFILE_RUN_ID="xpu-$tag" CAJETA_XPU_SHOWCASE_DIR="$DIG" \
            "$b" --run --area gfx --out "$csv.gfx" >>"$log" 2>&1 || rc=$?
    fi
    { cat "$csv.xpu"; tail -n +2 "$csv.gfx"; } > "$csv"
    echo "[xpu] leg $tag (backend $be) $((SECONDS - t0)) s, harness exit $rc"
    LEG_CSV="$csv"
}

# (2) cpu on every binary
for t in $BUILDS; do
    run_leg "$(bin_of "$t")" cpu "cpu-$t"
    $CHECK rows "$LEG_CSV" ok | sed "s/^/[xpu] /"
done

# (3) the consistency check fires
echo "saxpy 1/1 gemm 1/1" > "$DIG/vulkan.digest"
first="$(echo $BUILDS | awk '{print $1}')"
b="$(bin_of "$first")"
env CAJETA_BENCH_NO_GPU=1 CAJETA_XPU_BACKEND=cpu CAJETA_BENCH_SMOKE=1 CAJETA_XPU_SHOWCASE_DIR="$DIG" PATH="$SHIM:$PATH" \
    "$b" --run --area xpu --bench cross-backend-showcase --out "$OUTD/xpu-mismatch.csv" >"$OUTD/xpu-mismatch.log" 2>&1 || true
$CHECK invalid "$OUTD/xpu-mismatch.csv" cross-backend-showcase cpu | sed 's/^/[xpu] /'
rm -f "$DIG/vulkan.digest"

# (4) device backends
want="held"; [[ "$HELD" == 1 ]] || want="ok"
for t in $BUILDS; do
    case "$t" in
        gpu-nvptx)  run_leg "$(bin_of "$t")" cuda   "cuda";   $CHECK rows "$LEG_CSV" "$want" | tail -1 | sed 's/^/[xpu] cuda: /' ;;
        gpu-vulkan) run_leg "$(bin_of "$t")" vulkan "vulkan"; $CHECK rows "$LEG_CSV" "$want" | tail -1 | sed 's/^/[xpu] vulkan: /' ;;
    esac
done
if [[ ! -e "${ROCM_PATH:-/opt/rocm}/.info/version" ]]; then
    run_leg "$(bin_of "$first")" hip "hip"
    $CHECK rows "$LEG_CSV" norocm | tail -1 | sed 's/^/[xpu] hip: /'
fi

# (5) what the stubs caught
if [[ "$HELD" == 1 ]]; then
    echo "[xpu] driver-query attempts the stubs blocked: $(sort "$SHIM/attempts.log" | uniq -c | tr '\n' ' ')"
fi
echo "XPU OK ($(python3 "$SCRIPT_DIR/xpu_check.py" ids | wc -l) page ids; builds: $BUILDS; GPU held: $HELD)"
