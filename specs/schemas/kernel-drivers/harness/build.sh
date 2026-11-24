#!/bin/bash
# kernel-drivers gate harness: the lifted cajeta-llm reference kernels and ml.kernels' drivers on the same data.
#   build.sh <backend> <entry>     backend: cpu | amdgpu | vulkan | nvptx     entry: Bench, Bench2, Bench3, Bench5, Bench6 (cpu rows) | TileBench and the same numbers (GPU tiles)
#   CAJETA   compiler (cajeta 814f92e52 or later)
#   KERNELS  cajeta-ml checkout's kernels/ on branch kernel-drivers
#   UNIT_CJA dev.cajeta.unit archive
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
backend="$1"; entry="$2"
out="${OUT:-$here/out-$backend}"
rm -rf "$out/src"; mkdir -p "$out/src/dev" "$out/work"
cp -r "$here/ref" "$out/src/"
cp -r "$KERNELS/src/main/cajeta/dev/." "$out/src/dev/"
"${CAJETA:-cajeta}" --emit=exe --release --xpu-backend="$backend" --classpath="$UNIT_CJA" \
    -o "$out/$entry" "ref.$entry.main" "$out/src" "$out/work" > "$out/build.log" 2>&1
echo "built $out/$entry; run it with CAJETA_XPU_BACKEND=$backend"
