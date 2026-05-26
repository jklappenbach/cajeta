#!/usr/bin/env bash
# Build HelloWorld to an LLVM IR archive — one .ll per cajeta source
# under build/ir/ mirroring the package structure. Useful for inspecting
# the compiler's output, running a cross-target JIT, or feeding the IR
# into downstream LLVM tools (llvm-dis, opt, etc.).

set -euo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
REPO_ROOT="$( cd -- "${SCRIPT_DIR}/../.." &> /dev/null && pwd )"

CAJETA_BIN="${CAJETA_BIN:-${REPO_ROOT}/build/src/cajeta}"

SRC_ROOT="${SCRIPT_DIR}/src"
BUILD_DIR="${SCRIPT_DIR}/build/ir"

if [[ ! -x "$CAJETA_BIN" ]]; then
    echo "error: cajeta compiler not found at $CAJETA_BIN" >&2
    echo "       build the compiler first: cd $REPO_ROOT && ./build.sh" >&2
    exit 1
fi

ENTRY_METHOD="helloworld.HelloWorld.run"

rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"

echo "Compiling cajeta sources → LLVM IR (.ll)"
echo "  entry: $ENTRY_METHOD"
echo "  src:   $SRC_ROOT"
echo "  out:   $BUILD_DIR"
"$CAJETA_BIN" \
    --emit=ir \
    "$ENTRY_METHOD" \
    "$SRC_ROOT" \
    "$BUILD_DIR" \
    > "${BUILD_DIR}/cajeta-compile.log" 2>&1 || {
    echo "error: cajeta --emit=ir failed (see ${BUILD_DIR}/cajeta-compile.log)" >&2
    tail -20 "${BUILD_DIR}/cajeta-compile.log" >&2
    exit 1
}

echo ""
echo "IR files:"
find "$BUILD_DIR" -name '*.ll' -printf '  %p\n' | sort
