#!/bin/sh
# One 16x16x16 float16 cooperative-matrix multiply-accumulate on the cpu backend against the same
# product as row work-items, value-checked, on one core. Usage: run.sh <label>=<cajeta compiler> ... ;
# set CAJETA_GPU_COOPMATRIX_DIST=on to build the tile distributed across the wave.
set -e
D=$(cd "$(dirname "$0")" && pwd)
for spec in "$@"; do
    L="${spec%%=*}"; C="${spec#*=}"
    mkdir -p "$D/out-$L"
    "$C" --emit=exe --release --xpu-backend=cpu -o "$D/out-$L/t" bench.T.main "$D/src" "$D/out-$L" > "$D/out-$L/build.log" 2>&1
done
for pass in 1 2 3; do
    for spec in "$@"; do
        L="${spec%%=*}"
        echo "$pass $L: $(taskset -c 4 "$D/out-$L/t" | tr '\n' ' ')"
    done
done
