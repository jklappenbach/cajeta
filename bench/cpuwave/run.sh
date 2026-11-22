#!/bin/sh
# One int8 dot mat-vec (4096 x 4096) as a wave kernel and as a row-a-work-item kernel on the cpu
# backend, value-checked against each other. Usage: run.sh <label>=<cajeta compiler> ... ;
# builds each, then runs them interleaved, three passes.
set -e
D=$(cd "$(dirname "$0")" && pwd)
for spec in "$@"; do
    L="${spec%%=*}"; C="${spec#*=}"
    mkdir -p "$D/out-$L"
    "$C" --emit=exe --release --xpu-backend=cpu -o "$D/out-$L/w" bench.W.main "$D/src" "$D/out-$L" > "$D/out-$L/build.log" 2>&1
done
for pass in 1 2 3; do
    for spec in "$@"; do
        L="${spec%%=*}"
        echo "$pass $L: $("$D/out-$L/w" | tr '\n' ' ')"
    done
done
