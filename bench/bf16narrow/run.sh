#!/bin/sh
# bf16-cast-denormals 1.3.2: the cost of exact float32-to-bf16 narrowing on the cpu
# backend. Usage: run.sh <cajeta compiler> <label> [llvm-objdump]
# Run once with a compiler on cajeta-llvm r12 (the v0.53.11 release) and once with one
# on r13 (main), on an AVX512-BF16 CPU. It prints how many vcvtneps2bf16 the executable
# holds (r12: in every kernel; r13: in vecFast only), then each kernel's best time.
set -e
C="$1"; L="$2"; OBJDUMP="${3:-llvm-objdump}"
D=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$D/out-$L"
"$C" --emit=exe --release --xpu-backend=cpu -o "$D/out-$L/n" bench.N.main "$D/src" "$D/out-$L" > "$D/out-$L/build.log" 2>&1
echo "$L: vcvtneps2bf16 in the executable: $("$OBJDUMP" -d --no-show-raw-insn "$D/out-$L/n" | grep -c vcvtneps2bf16)"
"$D/out-$L/n"
