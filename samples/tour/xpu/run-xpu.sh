#!/usr/bin/env bash
# Compile + (optionally) run the Cajeta GPU tour.
#
# The same portable @Kernel source is compiled for whichever backend(s) you
# bundle and run through the runtime dispatcher:
#
#     ./run-xpu.sh                 # default: --xpu-backend=cpu (runs anywhere)
#     ./run-xpu.sh amdgpu,cpu      # use the AMD GPU, fall to CPU if absent
#     ./run-xpu.sh vulkan,cpu      # use a Vulkan device, fall to CPU if absent
#     ./run-xpu.sh nvptx,cpu       # use the NVIDIA GPU, fall to CPU if absent
#
# Knobs (environment):
#     XPU_BACKEND=<list>   same as the positional arg (arg wins if both given)
#     RUN=0                compile + link only, do not execute the binary
#     CAJETA_XPU_BACKEND=<one>   force the runtime's choice at execution time
#                                (e.g. bundle amdgpu,cpu but force cpu to prove
#                                 the fall-to-CPU path on a box that HAS the GPU)
#     DEBUG=1              keep symbols/debug info in the linked binary
#
# Steps (mirrors ../build-bin.sh, plus --xpu-backend):
#   1. cajeta --emit=obj --xpu-backend=<list> : per-module .o + the device
#      kernels, the registration ctors, and the backend manifest.
#   2. clang link the .o files (user code + embedded runtime) into a binary.
#   3. run it (unless RUN=0).

set -euo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
REPO_ROOT="$( cd -- "${SCRIPT_DIR}/../../.." &> /dev/null && pwd )"

CAJETA_BIN="${CAJETA_BIN:-${REPO_ROOT}/build/src/cajeta}"
CLANG_BIN="${CLANG_BIN:-clang-22}"

XPU_BACKEND="${1:-${XPU_BACKEND:-cpu}}"

SRC_ROOT="${SCRIPT_DIR}/src"
BUILD_DIR="${SCRIPT_DIR}/build/bin"
OUT_BINARY="${SCRIPT_DIR}/build/gpu-tour"
ENTRY_METHOD="tour.xpu.XpuTour.run"

if [[ ! -x "$CAJETA_BIN" ]]; then
    echo "error: cajeta compiler not found at $CAJETA_BIN" >&2
    echo "       build the compiler first: cd $REPO_ROOT && ./build.sh" >&2
    exit 1
fi

rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"

echo "[1/3] Compiling cajeta XPU sources → object files"
echo "      entry:       $ENTRY_METHOD"
echo "      xpu-backend: $XPU_BACKEND"
echo "      src:         $SRC_ROOT"
"$CAJETA_BIN" \
    --emit=obj \
    --xpu-backend="$XPU_BACKEND" \
    "$ENTRY_METHOD" \
    "$SRC_ROOT" \
    "$BUILD_DIR" \
    > "${BUILD_DIR}/cajeta-compile.log" 2>&1 || {
    echo "error: cajeta --emit=obj failed (see ${BUILD_DIR}/cajeta-compile.log)" >&2
    tail -20 "${BUILD_DIR}/cajeta-compile.log" >&2
    exit 1
}

mapfile -d '' OBJECTS < <(find "$BUILD_DIR" -name '*.o' -print0)
if [[ ${#OBJECTS[@]} -eq 0 ]]; then
    echo "error: cajeta produced no .o files (see ${BUILD_DIR}/cajeta-compile.log)" >&2
    exit 1
fi

echo "[2/3] Linking → $OUT_BINARY"
# The runtime dlopen's libcuda/libamdhip64/libvulkan on demand — none is a
# link-time dependency, so the binary links and runs on a box without any of
# them (the dispatcher then falls to the CPU if `cpu` was bundled). -ldl for
# dlopen; -lpthread/-lm for the runtime.
#
# cajeta_tls.o + -lssl -lcrypto: the stdlib object always references the
# `__cajeta_tls_*` natives (eager clinit global-constructors root every class,
# so --gc-sections cannot drop the unused TLS subsystem). `--emit=exe` links
# them automatically; this manual `--emit=obj` + clang link must add them.
# The compiled TLS native object sits next to the compiler binary.
TLS_OBJ="$(dirname "$CAJETA_BIN")/cajeta_tls.o"
if [[ ! -f "$TLS_OBJ" ]]; then
    echo "error: TLS native object not found at $TLS_OBJ" >&2
    echo "       (built alongside the compiler; rebuild with ./build.sh)" >&2
    exit 1
fi
# Weak OptiX stubs, same as --emit=exe generates: the AccelerationStructure
# noun references `cajeta_xpu_optix_*`; on a non-NVIDIA host `available()`
# returns 0 and the runtime takes the software-BVH path.
OPTIX_STUB="${BUILD_DIR}/__cajeta_xpu_optix_stub.c"
cat > "$OPTIX_STUB" <<'EOF'
#include <stdint.h>
#define W __attribute__((weak))
W int      cajeta_xpu_optix_available(void){return 0;}
W void*    cajeta_xpu_optix_context(void){return 0;}
W void*    cajeta_xpu_optix_cuda_context(void){return 0;}
W int64_t  cajeta_xpu_optix_accel_build_aabbs(const float*a,uint32_t b){(void)a;(void)b;return 0;}
W int64_t  cajeta_xpu_optix_accel_build_triangles(const float*a,uint32_t b,uint32_t c){(void)a;(void)b;(void)c;return 0;}
W uint64_t cajeta_xpu_optix_traversable(int64_t a){(void)a;return 0;}
W uint64_t cajeta_xpu_optix_accel_boxes(int64_t a){(void)a;return 0;}
W void     cajeta_xpu_optix_accel_free(int64_t a){(void)a;}
W int      cajeta_xpu_optix_launch(const char*a,uint64_t b,const char*c,const char*d,const char*e,const char*f,const void*g,uint64_t h,uint32_t i){(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h;(void)i;return -1;}
W int      cajeta_xpu_optix_launch_tri(const char*a,uint64_t b,const char*c,const char*d,const char*e,const char*f,const void*g,uint64_t h,uint32_t i){(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h;(void)i;return -1;}
EOF
LINK_FLAGS=( -Wl,--gc-sections )
if [[ "${DEBUG:-}" != "1" ]]; then
    LINK_FLAGS+=( -Wl,--strip-all )
fi
"$CLANG_BIN" \
    -o "$OUT_BINARY" \
    "${OBJECTS[@]}" \
    "$TLS_OBJ" \
    "$OPTIX_STUB" \
    -lssl \
    -lcrypto \
    -ldl \
    -lpthread \
    -lm \
    "${LINK_FLAGS[@]}"

echo "      built: $OUT_BINARY"

if [[ "${RUN:-1}" == "0" ]]; then
    echo "[3/3] RUN=0 — skipping execution"
    echo "      run it yourself: $OUT_BINARY"
    exit 0
fi

echo "[3/3] Running"
echo "---"
"$OUT_BINARY"
