#!/usr/bin/env bash
# xpu/babelstream-{copy,mul,add,triad,dot}: the BabelStream v5.0 reference
# builds (third_party/babelstream, pinned in pins.tsv), MODEL=cuda with nvcc
# (-arch=sm_89) on Phoenix and MODEL=hip on Proton, run at the Cajeta row's
# 2^25 float64 with --csv. BabelStream times its own kernels (100 iterations by
# default, PROFILE_TRIALS here) and validates its own result ("Validation
# failed" fails the check). Its CSV gives max MB/s, min, max and average
# runtime per kernel: min_ns is the min, median_ns and mean_ns BabelStream's
# average (it reports no median), p95_ns its max; throughput is its MB/s in B/s.
set -uo pipefail
source "$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )/../../lib" &> /dev/null && pwd )/xpu.sh"
SRC="$XPU_TP/babelstream"
IDS="copy mul add triad dot"
skip_all() { for k in $IDS; do xpu_skip "babelstream-$k" "" xpu "$1" "BabelStream" "$2" "$3"; done; }

build() {  # build <model> -> binary path or !reason
    local model="$1" b="$XPU_BUILD/babelstream-$1" bin="$XPU_BUILD/babelstream-$1/$1-stream"
    [[ -d "$SRC/src" ]] || { echo "!BabelStream checkout missing (competitors/setup.sh)"; return; }
    [[ -x "$bin" ]] && { echo "$bin"; return; }
    if [[ "$model" == cuda ]]; then
        local nvcc; nvcc="$(xpu_nvcc)" || { echo "!no nvcc under $XPU_CUDA"; return; }
        cmake -S "$SRC" -B "$b" -DMODEL=cuda -DCMAKE_CUDA_COMPILER="$nvcc" -DCUDA_ARCH=sm_89 -DCMAKE_BUILD_TYPE=Release >"$b.log" 2>&1 \
            && cmake --build "$b" -j 8 >>"$b.log" 2>&1 || { echo "!cmake build failed (see $b.log)"; return; }
    else
        local hipcc; hipcc="$(xpu_hipcc)" || { echo "!no ROCm on this machine"; return; }
        cmake -S "$SRC" -B "$b" -DMODEL=hip -DCMAKE_CXX_COMPILER="$hipcc" -DCMAKE_BUILD_TYPE=Release >"$b.log" 2>&1 \
            && cmake --build "$b" -j 8 >>"$b.log" 2>&1 || { echo "!cmake build failed (see $b.log)"; return; }
    fi
    [[ -x "$bin" ]] && echo "$bin" || echo "!no $1-stream binary after the build"
}

for model in cuda hip; do
    lang="cuda-c++"; [[ "$model" == hip ]] && lang="hip-c++"
    bin="$(build "$model")"
    ver="5.0"
    if [[ "$bin" == !* ]]; then skip_all "$lang" "$model" "${bin#!}"; continue; fi
    if xpu_build_only; then skip_all "$lang" "$model" "build-only: built (BabelStream 5.0 $model); not run"; continue; fi
    if xpu_held; then skip_all "$lang" "$model" "GPU held by another session (CAJETA_BENCH_NO_GPU=1)"; continue; fi
    n=$((1 << 25)); times="${PROFILE_TRIALS:-100}"; [[ "$times" -lt 2 ]] && times=2
    out="$("$bin" --arraysize "$n" --numtimes "$times" --csv 2>&1)"; rc=$?
    valid=true; grep -qi "validation failed" <<< "$out" && valid=false
    [[ $rc -eq 0 ]] || valid=false
    for k in $IDS; do
        line="$(awk -F, -v f="$(tr a-z A-Z <<< "${k:0:1}")${k:1}" '$1==f' <<< "$out" | head -1)"
        if [[ -z "$line" ]]; then xpu_skip "babelstream-$k" "" xpu "$lang" BabelStream "$model" "no $k line in BabelStream's output (rc $rc)"; continue; fi
        IFS=, read -r fn nt ne sz mbs tmin tmax tavg <<< "$line"
        arrays=2; [[ "$k" == add || "$k" == triad ]] && arrays=3
        awk -v id="babelstream-$k" -v n="$ne" -v a="$arrays" -v mbs="$mbs" -v tmin="$tmin" -v tmax="$tmax" -v tavg="$tavg" \
            -v nt="$nt" -v lang="$lang" -v be="$model" -v ok="$valid" -v rid="${PROFILE_RUN_ID:-local}" -v ts="${PROFILE_RUN_TS:-}" 'BEGIN {
            st = (ok == "true") ? "ok" : "invalid"
            printf "1,%s,%s,%s,xpu,,-1,,%d,%s,5.0,BabelStream,5.0,-O3 -arch=sm_89,1,%d,%d,%d,%d,%d,%.3f,B/s,-1,-1,-1,-1,-1,%s,%s,BabelStream 5.0 %s-stream --arraysize %d --numtimes %d: median and mean are its average; p95 its max,,%s\n",
                rid, ts, id, a * 8 * n, lang, nt, tmin * 1e9, tavg * 1e9, tavg * 1e9, tmax * 1e9, mbs * 1e6, st, ok, be, n, nt, be }'
    done
done
