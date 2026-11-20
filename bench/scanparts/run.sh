#!/bin/sh
# Which part of the xpu/scan kernel costs the time on a backend. Usage: run.sh <cajeta> <xpu-backend>
# [runtime backend], e.g. run.sh build/src/cajeta vulkan, or run.sh build/src/cajeta nvptx cuda.
set -e
C="$1"; B="$2"; R="${3:-$2}"
D=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$D/out-$B"
"$C" --emit=exe --release --xpu-backend="$B" -o "$D/out-$B/scan" bench.Scan.main "$D/src" "$D/out-$B" > "$D/out-$B/build.log" 2>&1
CAJETA_XPU_BACKEND="$R" "$D/out-$B/scan"
