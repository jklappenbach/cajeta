#!/usr/bin/env bash
# Build + run the matmul @Kernel sample (release, ThinLTO, CPU backend) and
# print its timing line. Usage: ./bench.sh [serial]
#   serial -> sets CAJETA_XPU_CPU_SERIAL=1 (single-thread baseline)
#
# CAJETA_XPU_CPU_WORKER_SPIN enables the hot, simultaneous-start worker pool
# (workers busy-wait for dispatch instead of sleeping on a condvar), which is
# what takes this n=200 float64 matmul UNDER OpenBLAS/numpy (~0.079 ms): we land
# ~0.057 ms. It is OFF by default in the runtime because true-simultaneous worker
# entry trips a latent high-concurrency race in workgroup-BARRIER (fission)
# kernels; matmul has no barriers, so it is safe here. See the runtime's
# caj_kpool spin-budget comment.
set -euo pipefail
export CAJETA_XPU_CPU_WORKER_SPIN="${CAJETA_XPU_CPU_WORKER_SPIN:-262144}"
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
CAJETA="$REPO/build-cajeta/src/cajeta"
OUT="$HERE/build/bench-out"
mkdir -p "$OUT"

"$CAJETA" \
    --emit=exe --xpu-backend=cpu --opt=O3 --lto=thin \
    -o "$OUT/mm" \
    mm.MatmulKernel.run "$HERE/src" "$OUT/arch" \
    > "$OUT/compile.log" 2>&1 || { echo "BUILD FAILED:"; tail -25 "$OUT/compile.log"; exit 1; }

if [[ "${1:-}" == "serial" ]]; then
    CAJETA_XPU_CPU_SERIAL=1 "$OUT/mm"
else
    "$OUT/mm"
fi
