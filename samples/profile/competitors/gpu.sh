#!/usr/bin/env bash
# GPU matmul competitor area runner. Runs each present GPU library's dense f64
# matmul (same sizes + identity-A init as the Cajeta gpu bench, so C==B is an
# exact cross-check) and streams schema CSV rows (columns.txt order + trailing
# `backend`) to stdout. PyTorch ROCm (rocBLAS-backed) is the headline GPU peer.
# Missing venv/lib -> `skipped` rows (never a hard failure).
#
# Env: PROFILE_RUN_ID, PROFILE_RUN_TS, PROFILE_LANGS (space-list filter),
#      PROFILE_GPU_PY (a python with a WORKING ROCm torch; the shared
#      ml/venv-rocm7.x is too old and segfaults on gfx1151 — use the pinned
#      ml/venv-rocm-gfx1151, see specs/gpu-vulkan-f64-spec / memory).
set -uo pipefail

DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
LANGS="${PROFILE_LANGS:-python cpp}"
SIZES="256 512 1024 2048"
want() { case " $LANGS " in *" $1 "*) return 0;; *) return 1;; esac; }

skip_gpu() {  # $1=lang $2=lib $3=reason
    local lang="$1" lib="$2" reason="$3" rid="${PROFILE_RUN_ID:-local}" ts="${PROFILE_RUN_TS:-}" n sz
    for n in $SIZES; do sz=$((n * n))
        echo "1,${rid},${ts},matmul,gpu,,-1,,${sz},${lang},,${lib},,,0,0,-1,-1,-1,-1,,,-1,-1,-1,-1,-1,skipped,,n${n},hip"
    done
}

# ---- python: PyTorch ROCm (gfx1151 / HIP, rocBLAS) — headline GPU peer ----
if want python; then
    PY="${PROFILE_GPU_PY:-/home/julian/code/ml/venv-rocm-gfx1151/bin/python3}"
    if [[ -x "$PY" ]]; then
        PROFILE_TS="${PROFILE_RUN_TS:-}" "$PY" "$DIR/python/gpu/matmul_bench.py" 2>/dev/null \
            || skip_gpu python torch "pytorch gpu runner crashed"
    else
        skip_gpu python torch "no ROCm torch venv (set PROFILE_GPU_PY)"
    fi
fi

# ---- cpp: rocBLAS (rocblas_dgemm) — second GPU peer (plan U4 4.2.b) ----
# gfx1151 needs a newer rocBLAS than older system builds (which segfault on this arch); run
# against the gfx1151 venv's rocm libs (the same stack PyTorch uses) via
# LD_LIBRARY_PATH. Override with PROFILE_GPU_ROCM_LIB.
if want cpp; then
    ROCBLAS="$DIR/cpp/gpu/matmul_rocblas.cpp"
    GPU_ROCM_LIB="${PROFILE_GPU_ROCM_LIB:-$(find /home/julian/code/ml/venv-rocm-gfx1151/lib/python3.12/site-packages -path '*_rocm_sdk*/lib' -type d 2>/dev/null | tr '\n' ':')}"
    if [[ -f "$ROCBLAS" ]] && command -v hipcc >/dev/null 2>&1; then
        BIN="$DIR/.build/gpu-rocblas"; mkdir -p "$DIR/.build"
        if hipcc -O3 "$ROCBLAS" -lrocblas -o "$BIN" >/dev/null 2>&1; then
            LD_LIBRARY_PATH="${GPU_ROCM_LIB}${LD_LIBRARY_PATH:-}" "$BIN" \
                || skip_gpu cpp rocBLAS "rocblas runner crashed"
        else
            skip_gpu cpp rocBLAS "hipcc/rocblas build failed"
        fi
    else
        skip_gpu cpp rocBLAS "hipcc or rocblas competitor not present"
    fi
fi
