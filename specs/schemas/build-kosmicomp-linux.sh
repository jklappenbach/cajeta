#!/usr/bin/env bash
# Build KosmicKrisp's SPIR-V -> MSL compiler ON LINUX (kosmickrisp-upstream 1.2.1/1.2.2).
#
# KosmicKrisp is a macOS driver, but only `src/kosmickrisp/bridge/` is Metal —
# `libmsl_compiler` depends on nothing but NIR, mesautil and vulkan_runtime.
# So the MSL the backend emits can be developed and asserted with no Mac.
#
# Meson will not do this for us: `-Dvulkan-drivers=kosmickrisp` forces
# with_clc -> LLVMSPIRVLib, which is a hard dependency of the CL path we do not
# need. So configure a driver we can build (swrast) for NIR/vtn/mesautil, then
# compile the eight KosmicKrisp compiler translation units against it by hand.
#
# Usage: build-kosmicomp-linux.sh [mesa-src] [out-dir]

set -euo pipefail

MESA="${1:-$HOME/code/mesa}"
OUT="${2:-${TMPDIR:-/tmp}/kosmicomp-linux}"
BUILD="$OUT/mesa-build"

# mako lives in the system python; a pyenv shim usually does not have it.
PY=/usr/bin/python3
command -v "$PY" >/dev/null || PY=$(command -v python3)

mkdir -p "$OUT"

if [[ ! -f "$BUILD/build.ninja" ]]; then
    meson setup "$BUILD" "$MESA" \
        -Dvulkan-drivers=swrast -Dgallium-drivers=llvmpipe \
        -Dplatforms= -Dglx=disabled -Degl=disabled -Dgbm=disabled
fi

ninja -C "$BUILD" \
    src/compiler/nir/libnir.a \
    src/compiler/spirv/libvtn.a \
    src/compiler/libcompiler.a \
    src/util/libmesa_util.a \
    src/util/blake3/libblake3.a

# Reuse meson's own flags for a NIR translation unit — they carry the ~90
# HAVE_* defines and generated-header include paths that NIR will not build
# without, and they change between mesa versions.
cflags() {
    "$PY" - "$BUILD" <<'PY'
import json, shlex, os, sys
b = sys.argv[1]
cc = json.load(open(b + "/compile_commands.json"))
base = next(shlex.split(e['command']) for e in cc
            if e['file'].endswith('/nir.c') and '/compiler/nir/' in e['file'])
out, skip = [], False
for x in base[2:]:
    if skip:
        skip = False; continue
    if x in ('-MQ', '-MF', '-o'):
        skip = True; continue
    if x in ('-MD', '-c') or x.endswith('.c') or x.endswith('.o'):
        continue
    out.append('-I' + os.path.normpath(os.path.join(b, x[2:]))
               if x.startswith('-I') and not os.path.isabs(x[2:]) else x)
print(' '.join(out))
PY
}

IFS=' ' read -r -a FLAGS <<< "$(cflags)"
FLAGS+=(-I"$MESA/src/kosmickrisp/compiler" -I"$MESA/src/vulkan/runtime"
        -I"$MESA/src/vulkan/util" -I"$BUILD/src/vulkan/runtime"
        -I"$BUILD/src/compiler" -Wno-error -Wno-error=vla -Wno-vla)

# nir_algebraic needs mako, and emits the generated pass meson would build.
"$PY" "$MESA/src/kosmickrisp/compiler/msl_nir_algebraic.py" \
    -p "$MESA/src/compiler/nir" > "$OUT/msl_nir_algebraic.c"

SRCS=(nir_to_msl msl_type_inference msl_iomap msl_nir_gather_common
      msl_nir_lower_common msl_nir_lower_input_attachments msl_nir_lower_subgroups)
OBJS=()
for s in "${SRCS[@]}"; do
    cc "${FLAGS[@]}" -c "$MESA/src/kosmickrisp/compiler/$s.c" -o "$OUT/$s.o"
    OBJS+=("$OUT/$s.o")
done
cc "${FLAGS[@]}" -c "$OUT/msl_nir_algebraic.c" -o "$OUT/msl_nir_algebraic.o"
cc "${FLAGS[@]}" -c "$MESA/src/kosmickrisp/kosmicomp.c" -o "$OUT/kosmicomp.o"

cc -o "$OUT/kosmicomp" "$OUT/kosmicomp.o" "${OBJS[@]}" "$OUT/msl_nir_algebraic.o" \
    -Wl,--start-group \
        "$BUILD/src/compiler/nir/libnir.a" \
        "$BUILD/src/compiler/spirv/libvtn.a" \
        "$BUILD/src/util/libmesa_util.a" \
        "$BUILD/src/compiler/libcompiler.a" \
        "$BUILD/src/util/blake3/libblake3.a" \
    -Wl,--end-group \
    -lSPIRV-Tools -lz -lzstd -lexpat -lpthread -lm -ldl -lstdc++

echo "$OUT/kosmicomp"
