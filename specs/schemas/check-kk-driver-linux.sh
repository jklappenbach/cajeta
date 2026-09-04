#!/usr/bin/env bash
# Syntax-check a KosmicKrisp DRIVER source on Linux (kosmickrisp-upstream 2.x).
#
# The driver half calls into `bridge/` (Objective-C/Metal), but its headers are
# plain C declarations — so `cc -fsyntax-only` type-checks the whole file
# without a Mac. That catches the entire class of error this work actually
# produces: a field that does not exist in vk_features / vk_properties, a
# mistyped extension-table entry, a bad entry-point signature.
#
# Needs the generated headers meson would produce. build-kosmicomp-linux.sh
# leaves the mesa build tree this reuses.
#
# Usage: check-kk-driver-linux.sh <file.c> [mesa-src] [kosmicomp-out-dir]

set -euo pipefail

SRC="${1:?usage: check-kk-driver-linux.sh <file.c> [mesa-src] [out-dir]}"
MESA="${2:-$HOME/code/mesa}"
OUT="${3:-${TMPDIR:-/tmp}/kosmicomp-linux}"
BUILD="$OUT/mesa-build"
GEN="$OUT/gen"

PY=/usr/bin/python3
command -v "$PY" >/dev/null || PY=$(command -v python3)

[[ -f "$BUILD/build.ninja" ]] || {
    echo "no mesa build tree at $BUILD — run build-kosmicomp-linux.sh first" >&2
    exit 1
}

# Generated headers: the vulkan dispatch table, and KosmicKrisp's own
# entry-point prototypes (which is where a new VKAPI_ATTR function must match).
ninja -C "$BUILD" src/vulkan/util/vk_dispatch_table.h \
                  src/vulkan/runtime/libvulkan_lite_runtime.a >/dev/null

mkdir -p "$GEN"
[[ -f "$GEN/kk_entrypoints.h" ]] || \
    "$PY" "$MESA/src/vulkan/util/vk_entrypoints_gen.py" \
        --xml "$MESA/src/vulkan/registry/vk.xml" --proto --weak \
        --out-h "$GEN/kk_entrypoints.h" --out-c "$GEN/kk_entrypoints.c" \
        --prefix kk --beta false
printf '#define MESA_GIT_SHA1 "git-devel"\n' > "$GEN/git_sha1.h"

# Reuse meson's flags for a vulkan_runtime TU — they carry the HAVE_* defines
# c11/threads.h refuses to build without.
mapfile -t FLAGS < <("$PY" - "$BUILD" <<'PY'
import json, shlex, os, sys
b = sys.argv[1]
cc = json.load(open(b + "/compile_commands.json"))
base = next((shlex.split(e['command']) for e in cc
             if '/vulkan/runtime/vk_device.c' in e['file']), None)
if base is None:
    base = next(shlex.split(e['command']) for e in cc
                if '/vulkan/runtime/' in e['file'] and e['file'].endswith('.c'))
skip = False
for x in base[2:]:
    if skip:
        skip = False; continue
    if x in ('-MQ', '-MF', '-o'):
        skip = True; continue
    if x in ('-MD', '-c') or x.endswith('.c') or x.endswith('.o'):
        continue
    print('-I' + os.path.normpath(os.path.join(b, x[2:]))
          if x.startswith('-I') and not os.path.isabs(x[2:]) else x)
PY
)

cc -fsyntax-only "${FLAGS[@]}" \
    -I"$BUILD/src/vulkan/util" -I"$MESA/src/vulkan/wsi" -I"$GEN" \
    -I"$MESA/src/kosmickrisp/vulkan" -I"$MESA/src/kosmickrisp/bridge" \
    -I"$MESA/src/kosmickrisp" -I"$MESA/src/kosmickrisp/compiler" \
    -Wno-error "$SRC"

echo "OK: $SRC"
