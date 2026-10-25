#!/usr/bin/env bash
# xpu/mesh-simplify: meshoptimizer v0.25 (third_party/meshoptimizer, pinned in
# pins.tsv) meshopt_simplify on the Cajeta row's grid height field, CPU only
# (it is the CPU reference the page names), built from source with g++ -O3
# -march=native. Runs even under PROFILE_BUILD_ONLY and CAJETA_BENCH_NO_GPU:
# nothing here touches a device. Rows are v1 (run.sh makes them v2).
set -uo pipefail
source "$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )/../../lib" &> /dev/null && pwd )/xpu.sh"
MO="$XPU_TP/meshoptimizer"
SRC="$XPU_DIR/cpp/xpu/meshopt/meshopt_bench.cpp"
BIN="$XPU_BUILD/meshopt_bench"
v="1e6-to-1e5 19800-to-1980"; [[ "${CAJETA_BENCH_SMOKE:-}" == 1 ]] && v="1e6-to-1e5 2048-to-204"
if [[ ! -f "$MO/src/meshoptimizer.h" ]]; then
    for x in $v; do xpu_skip mesh-simplify "$x" xpu cpp meshoptimizer cpu "meshoptimizer checkout missing (competitors/setup.sh)"; done; exit 0
fi
if [[ ! -x "$BIN" || "$SRC" -nt "$BIN" ]]; then
    g++ -O3 -march=native -std=c++17 -I"$MO/src" "$SRC" "$MO"/src/*.cpp -o "$BIN" >"$BIN.log" 2>&1 \
        || { for x in $v; do xpu_skip mesh-simplify "$x" xpu cpp meshoptimizer cpu "g++ build failed (see $BIN.log)"; done; exit 0; }
fi
PROFILE_WARMUP="${PROFILE_WARMUP:-1}" PROFILE_TRIALS="${PROFILE_TRIALS:-5}" "$BIN"
