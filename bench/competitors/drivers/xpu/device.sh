#!/usr/bin/env bash
# xpu/ device competitors in CUDA C++ and HIP C++ (benchmark-reference unit 14,
# spec §6.13): competitors/cuda/xpu/devserver, one workload per catalog id with
# the Cajeta row's sizes, inputs and checks; cuBLAS (GEMM), CUB (reduce, scan),
# plain kernels and the runtime API elsewhere. Built with nvcc -O3 -arch=sm_89
# (CUDA 13.3 on Phoenix); the HIP build is the same source through hipcc on
# Proton. Rows are v1 (32 columns); run.sh makes them v2 with catalog ids. gfx
# rows (texture-sample-rate) are dropped here and printed by drivers/gfx/device.sh.
# The same binary is the interleaved competitor of the Cajeta harness
# (CAJETA_XPU_COMPETITOR=<path>/devserver-cuda, see bench/src/main/cajeta/bench/xpu/XpuCompetitor.cajeta).
set -uo pipefail
source "$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )/../../lib" &> /dev/null && pwd )/xpu.sh"
AREA="${XPU_DEVICE_AREA:-xpu}"

for be in cuda hip; do
    lang="cuda-c++"; [[ "$be" == hip ]] && lang="hip-c++"
    bin="$(xpu_build_devserver "$be")"
    ver=""; [[ "$be" == cuda ]] && ver="CUDA $(xpu_cuda_version)"
    # The id list comes from the CUDA build's --list (no device touch) or, for a
    # backend that did not build, from the source's ALL table.
    lister="$bin"; [[ "$bin" == !* ]] && lister="$XPU_BUILD/devserver-cuda"
    if [[ -x "$lister" ]]; then
        ids="$("$lister" --list)"
    else
        ids="$(sed -n '/static const char\* ALL\[\]/,/};/p' "$XPU_DIR/cuda/xpu/devserver/devserver.cu" | grep -o '"[a-z0-9-]*"' | tr -d '"' | sed 's/^texture-sample-rate$/gfx\/&/; /^gfx/!s/^/xpu\//')"
    fi
    reason=""
    if [[ "$bin" == !* ]]; then
        reason="${bin#!}"
    elif xpu_build_only; then reason="build-only: built ($ver); not run"
    elif xpu_held; then reason="GPU held by another session (CAJETA_BENCH_NO_GPU=1)"
    fi
    if [[ -n "$reason" ]]; then
        while read -r path variant; do
            [[ -n "$path" ]] || continue
            a="${path%%/*}"; id="${path#*/}"
            [[ "$a" == "$AREA" ]] || continue
            xpu_skip "$id" "${variant:-}" "$a" "$lang" "devserver" "$be" "$reason"
        done <<< "$ids"
        continue
    fi
    want=()
    while read -r path variant; do
        a="${path%%/*}"; id="${path#*/}"
        [[ "$a" == "$AREA" ]] && want+=("$id")
    done <<< "$ids"
    mapfile -t uniq < <(printf '%s\n' "${want[@]}" | awk '!seen[$0]++')
    PROFILE_WARMUP="${PROFILE_WARMUP:-3}" PROFILE_TRIALS="${PROFILE_TRIALS:-20}" "$bin" --rows "${uniq[@]}" \
        || echo "device.sh: devserver-$be exited non-zero" >&2
done
