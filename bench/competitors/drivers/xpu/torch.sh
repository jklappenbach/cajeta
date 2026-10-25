#!/usr/bin/env bash
# xpu/ PyTorch rows (python/xpu/torch_xpu/torch_xpu.py): torch.cuda on Phoenix
# (the competitors venv's torch 2.14.1+cu130), the ROCm wheel on Proton
# (CAJETA_XPU_BACKEND=hip). PROFILE_BUILD_ONLY=1: py_compile only, nothing
# imported. CAJETA_BENCH_NO_GPU=1: the script prints skipped rows without
# importing torch, so no torch.cuda call is made.
set -uo pipefail
source "$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )/../../lib" &> /dev/null && pwd )/xpu.sh"
PYF="$XPU_DIR/python/xpu/torch_xpu/torch_xpu.py"
PY="${PROFILE_GPU_PY:-$XPU_VENV/bin/python}"
IDS="device-saxpy wave-reduce scan gemm-f64 gemm-f32 gemm-f16 gemm-bf16 host-device-transfer kernelbuffer-alloc-free"
be=cuda; [[ "${CAJETA_XPU_BACKEND:-}" == hip || "${CAJETA_XPU_BACKEND:-}" == amdgpu ]] && be=hip
var() { case "$1" in gemm-*) echo n4096 ;; *) echo "" ;; esac; }
tv="$(ls -d "$XPU_VENV"/lib/python3*/site-packages/torch-*.dist-info 2>/dev/null | head -1 | sed -E 's/.*torch-(.*)\.dist-info/\1/')"
if xpu_build_only; then
    if python3 -m py_compile "$PYF" 2>/dev/null; then r="build-only: compiled (torch $tv); not run"; else r="build-only: py_compile failed"; fi
    for id in $IDS; do xpu_skip "$id" "$(var "$id")" xpu python torch "$be" "$r"; done
    exit 0
fi
if [[ ! -x "$PY" ]]; then
    for id in $IDS; do xpu_skip "$id" "$(var "$id")" xpu python torch "$be" "no python at $PY (competitors/setup.sh)"; done
    exit 0
fi
"$PY" "$PYF" || { for id in $IDS; do xpu_skip "$id" "$(var "$id")" xpu python torch "$be" "torch_xpu.py exited non-zero"; done; }
