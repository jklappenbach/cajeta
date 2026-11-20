#!/bin/sh
# The cost of a cpu-backend launch and sync (a trivial kernel, 1000 launches a round, best of
# five) at four sizes. Usage: run.sh <label>=<cajeta compiler> ... ; builds each, then runs
# them interleaved, three passes. Compare builds of different releases on one machine.
set -e
D=$(cd "$(dirname "$0")" && pwd)
for spec in "$@"; do
    L="${spec%%=*}"; C="${spec#*=}"
    mkdir -p "$D/out-$L"
    "$C" --emit=exe --release --xpu-backend=cpu -o "$D/out-$L/l" bench.L.main "$D/src" "$D/out-$L" > "$D/out-$L/build.log" 2>&1
done
for pass in 1 2 3; do
    for spec in "$@"; do
        L="${spec%%=*}"
        echo "$pass $L: $("$D/out-$L/l" | tr '\n' ' ')"
    done
done
