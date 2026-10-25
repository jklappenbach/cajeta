#!/usr/bin/env bash
# Shared by the xpu/ and gfx/ competitor drivers (benchmark-reference unit 14):
# where each device program is built, how it is built, and the skipped rows a
# driver prints for every id it would have measured. Sourced, not run.
#
#   PROFILE_BUILD_ONLY=1   build every program, run none (every row skipped,
#                          "build-only: built; not run" or the build failure)
#   CAJETA_BENCH_NO_GPU=1  build, then skip every device row ("GPU held by
#                          another session"); no program touches the device
#   CUDA_PATH              the CUDA toolkit (default /usr/local/cuda; nvcc and
#                          ptxas 13.3 on Phoenix)
#   ROCM_PATH              the ROCm install (default /opt/rocm); absent: every
#                          hip row is skipped "no ROCm on this machine"
#
# Nothing here queries a device: the toolkit versions come from the compilers.

XPU_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )/.." &> /dev/null && pwd )"   # competitors/
XPU_BUILD="$XPU_DIR/.build/xpu"
XPU_TP="$XPU_DIR/third_party"
XPU_CUDA="${CUDA_PATH:-/usr/local/cuda}"
XPU_ROCM="${ROCM_PATH:-/opt/rocm}"
XPU_VENV="$XPU_DIR/python/.venv"
mkdir -p "$XPU_BUILD"

xpu_nvcc() { [[ -x "$XPU_CUDA/bin/nvcc" ]] && echo "$XPU_CUDA/bin/nvcc"; }
xpu_hipcc() { if [[ -x "$XPU_ROCM/bin/hipcc" ]]; then echo "$XPU_ROCM/bin/hipcc"; elif command -v hipcc >/dev/null 2>&1; then command -v hipcc; else return 1; fi; }
xpu_cuda_version() { local n; n="$(xpu_nvcc)" || return; "$n" --version | sed -nE 's/.*release ([0-9.]+), V([0-9.]+).*/\2/p'; }
xpu_held() { [[ "${CAJETA_BENCH_NO_GPU:-}" == "1" || "${CAJETA_BENCH_NO_GPU:-}" == "true" ]]; }
xpu_build_only() { [[ "${PROFILE_BUILD_ONLY:-}" == "1" ]]; }

# cuBLAS on Phoenix is the nvidia-cublas wheel the competitors venv's torch
# pulled (CUDA 13 wheels; the toolkit under /usr/local/cuda ships no cuBLAS).
# Only its cublas*.h are put on the include path: the wheel's own cuda_runtime
# headers are 13.0 and would shadow the 13.3 toolkit's.
xpu_cublas_dir() { ls -d "$XPU_VENV"/lib/python3*/site-packages/nvidia/cu13 2>/dev/null | head -1; }
xpu_cublas_version() {
    ls -d "$XPU_VENV"/lib/python3*/site-packages/nvidia_cublas-*.dist-info 2>/dev/null | head -1 | sed -E 's/.*nvidia_cublas-([0-9.]+)\.dist-info/\1/'
}

# xpu_skip <id> <variant> <area> <lang> <library> <backend> <reason>  -- one v1 skipped row
xpu_skip() {
    local reason="${7//,/;}"
    echo "1,${PROFILE_RUN_ID:-local},${PROFILE_RUN_TS:-},$1,$3,,-1,,-1,$4,,$5,,,0,0,-1,-1,-1,-1,,,-1,-1,-1,-1,-1,skipped,,${reason},$2,$6"
}

# Build the devserver for cuda (nvcc) or hip (hipcc); prints the binary or a
# reason starting with "!" on failure. Rebuilt when the source is newer.
xpu_build_devserver() {
    local be="$1" src="$XPU_DIR/cuda/xpu/devserver" out="$XPU_BUILD/devserver-$1" log="$XPU_BUILD/devserver-$1.log"
    if [[ -x "$out" && "$out" -nt "$src/devserver.cu" && "$out" -nt "$src/gpu_compat.h" ]]; then echo "$out"; return; fi
    if [[ "$be" == cuda ]]; then
        local nvcc cb; nvcc="$(xpu_nvcc)" || { echo "!no nvcc under $XPU_CUDA"; return; }
        cb="$(xpu_cublas_dir)"; [[ -n "$cb" ]] || { echo "!no cuBLAS (nvidia-cublas wheel) in the competitors venv"; return; }
        local inc="$XPU_BUILD/cublas-include"; mkdir -p "$inc"
        for h in "$cb"/include/cublas*.h; do ln -sf "$h" "$inc/"; done
        if "$nvcc" -O3 -std=c++17 -arch=sm_89 -I"$inc" "$src/devserver.cu" -o "$out" \
                -L"$cb/lib" -l:libcublas.so.13 -l:libcublasLt.so.13 -Xlinker -rpath -Xlinker "$cb/lib" >"$log" 2>&1; then
            echo "$out"
        else
            echo "!nvcc build failed (see $log)"
        fi
    else
        local hipcc; hipcc="$(xpu_hipcc)" || { echo "!no ROCm on this machine"; return; }
        if "$hipcc" -O3 -std=c++17 -x hip "$src/devserver.cu" -o "$out" -lhipblas >"$log" 2>&1; then
            echo "$out"
        else
            echo "!hipcc build failed (see $log)"
        fi
    fi
}
