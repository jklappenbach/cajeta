#!/usr/bin/env bash
# Build HelloWorld to a native binary for the current architecture.
#
# Steps:
#   1. Run the cajeta compiler with --emit=obj to produce a .o per source.
#   2. Link the per-module .o files (user code + stdlib runtime) into an
#      executable using the system C toolchain (clang-22).
#
# The C toolchain link step is necessary because the compiler in this
# repo isn't built against lld libraries today, so --emit=exe's in-
# process link path falls back to a hint. Once lld-N-dev is installed
# and the compiler reconfigured, --emit=exe will work directly.

set -euo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
REPO_ROOT="$( cd -- "${SCRIPT_DIR}/../.." &> /dev/null && pwd )"

CAJETA_BIN="${CAJETA_BIN:-${REPO_ROOT}/build/src/cajeta}"
CLANG_BIN="${CLANG_BIN:-clang-22}"

SRC_ROOT="${SCRIPT_DIR}/src"
BUILD_DIR="${SCRIPT_DIR}/build/bin"
OUT_BINARY="${SCRIPT_DIR}/build/HelloWorld"

if [[ ! -x "$CAJETA_BIN" ]]; then
    echo "error: cajeta compiler not found at $CAJETA_BIN" >&2
    echo "       build the compiler first: cd $REPO_ROOT && ./build.sh" >&2
    exit 1
fi

ENTRY_METHOD="helloworld.HelloWorld.run"

rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"

echo "[1/2] Compiling cajeta sources → object files"
echo "      entry: $ENTRY_METHOD"
echo "      src:   $SRC_ROOT"
echo "      out:   $BUILD_DIR"
"$CAJETA_BIN" \
    --emit=obj \
    "$ENTRY_METHOD" \
    "$SRC_ROOT" \
    "$BUILD_DIR" \
    > "${BUILD_DIR}/cajeta-compile.log" 2>&1 || {
    echo "error: cajeta --emit=obj failed (see ${BUILD_DIR}/cajeta-compile.log)" >&2
    tail -20 "${BUILD_DIR}/cajeta-compile.log" >&2
    exit 1
}

# Collect every .o the compiler produced (one per cajeta source plus the
# stdlib's combined runtime + parsed stdlib classes).
mapfile -d '' OBJECTS < <(find "$BUILD_DIR" -name '*.o' -print0)
if [[ ${#OBJECTS[@]} -eq 0 ]]; then
    echo "error: cajeta produced no .o files (see ${BUILD_DIR}/cajeta-compile.log)" >&2
    exit 1
fi

echo "[2/2] Linking → $OUT_BINARY"
# --gc-sections drops stdlib functions / globals nothing references
# (JSON codec, hash families, parallel-stream driver, etc.) — only
# works because the cajeta TargetMachine emits one ELF section per
# function + data global (FunctionSections / DataSections).
# --strip-all drops debug + symbol tables. Set DEBUG=1 to keep them
# (e.g. DEBUG=1 ./build-bin.sh produces a debuggable binary with
# named stack frames).
LINK_FLAGS=( -Wl,--gc-sections )
if [[ "${DEBUG:-}" != "1" ]]; then
    LINK_FLAGS+=( -Wl,--strip-all )
fi
"$CLANG_BIN" \
    -o "$OUT_BINARY" \
    "${OBJECTS[@]}" \
    -lpthread \
    -lm \
    "${LINK_FLAGS[@]}"

echo ""
echo "built: $OUT_BINARY"
echo "run:   $OUT_BINARY"
