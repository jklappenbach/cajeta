#!/usr/bin/env bash
# xpu/mixbench-sweep: the mixbench reference builds (third_party/mixbench,
# pinned): mixbench-cuda (nvcc, sm_89) on Phoenix, mixbench-hip on Proton.
# mixbench runs its own sweep over compute iterations 0..1024 on a 32M-element
# buffer, one kernel launch per point timed with events; the rows here are its
# single-precision points at the Cajeta variants ci0..ci256 (min = median =
# mean = p95 = its one timing), throughput its GFLOP/s in flop/s. mixbench has
# no correctness output; check_ok is that the run exited 0 and printed the point.
set -uo pipefail
source "$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )/../../lib" &> /dev/null && pwd )/xpu.sh"
CIS="0 1 2 4 8 16 32 64 128 256"
skip_all() { for c in $CIS; do xpu_skip mixbench-sweep "ci$c" xpu "$1" mixbench "$2" "$3"; done; }

build() {
    local m="$1" b="$XPU_BUILD/mixbench-$1" bin="$XPU_BUILD/mixbench-$1/mixbench-$1"
    [[ -d "$XPU_TP/mixbench/mixbench-$m" ]] || { echo "!mixbench checkout missing (competitors/setup.sh)"; return; }
    [[ -x "$bin" ]] && { echo "$bin"; return; }
    if [[ "$m" == cuda ]]; then
        local nvcc; nvcc="$(xpu_nvcc)" || { echo "!no nvcc under $XPU_CUDA"; return; }
        cmake -S "$XPU_TP/mixbench/mixbench-cuda" -B "$b" -DCMAKE_CUDA_COMPILER="$nvcc" -DCMAKE_CUDA_ARCHITECTURES=89 \
            -DCMAKE_BUILD_TYPE=Release >"$b.log" 2>&1 && cmake --build "$b" -j 8 >>"$b.log" 2>&1 || { echo "!cmake build failed (see $b.log)"; return; }
    else
        local hipcc; hipcc="$(xpu_hipcc)" || { echo "!no ROCm on this machine"; return; }
        cmake -S "$XPU_TP/mixbench/mixbench-hip" -B "$b" -DCMAKE_CXX_COMPILER="$hipcc" -DCMAKE_BUILD_TYPE=Release >"$b.log" 2>&1 \
            && cmake --build "$b" -j 8 >>"$b.log" 2>&1 || { echo "!cmake build failed (see $b.log)"; return; }
    fi
    [[ -x "$bin" ]] && echo "$bin" || echo "!no mixbench-$m binary after the build"
}

for m in cuda hip; do
    lang="cuda-c++"; [[ "$m" == hip ]] && lang="hip-c++"
    bin="$(build "$m")"
    if [[ "$bin" == !* ]]; then skip_all "$lang" "$m" "${bin#!}"; continue; fi
    if xpu_build_only; then skip_all "$lang" "$m" "build-only: built (mixbench 06297d6 $m); not run"; continue; fi
    if xpu_held; then skip_all "$lang" "$m" "GPU held by another session (CAJETA_BENCH_NO_GPU=1)"; continue; fi
    out="$("$bin" 2>&1)"; rc=$?
    for c in $CIS; do
        # CSV point: compute_iters, sp ops/byte, sp ms, sp GFLOP/s, sp GB/s, dp ..., hp ..., int ...
        line="$(awk -F, -v c="$c" '{g=$1; gsub(/ /,"",g)} g==c && NF>=5 {print; exit}' <<< "$out")"
        if [[ -z "$line" || $rc -ne 0 ]]; then xpu_skip mixbench-sweep "ci$c" xpu "$lang" mixbench "$m" "no ci=$c point in mixbench output (rc $rc)"; continue; fi
        awk -F, -v c="$c" -v lang="$lang" -v be="$m" -v rid="${PROFILE_RUN_ID:-local}" -v ts="${PROFILE_RUN_TS:-}" '{
            ms = $3 + 0; gf = $4 + 0; ns = ms * 1e6
            n = 32 * 1024 * 1024; t = n / 32; ops = (8 * t + 16 * c * t) * 4
            printf "1,%s,%s,mixbench-sweep,xpu,,-1,,%d,%s,06297d6,mixbench,06297d6,-O3,0,1,%d,%d,%d,%d,%.3f,flop/s,-1,-1,-1,-1,-1,ok,true,mixbench %s single precision point ci=%d (one event-timed launch; ops/byte %s),ci%d,%s\n",
                rid, ts, ops, lang, ns, ns, ns, ns, gf * 1e9, be, c, $2, c, be }' <<< "$line"
    done
done
