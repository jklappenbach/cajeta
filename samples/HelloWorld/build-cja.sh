#!/usr/bin/env bash
# Build HelloWorld into a project-only cajeta archive (.cja). Bundles
# the user's compiled bitcode into a single file with a manifest —
# stdlib is NOT included (consumers bring their own via --classpath).
# The library distribution shape.
#
# See cajeta-docs/Compilation.md § Output formats for the contract and
# § Uber archives for the runnable, self-contained form (build-uber.sh).

set -euo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
REPO_ROOT="$( cd -- "${SCRIPT_DIR}/../.." &> /dev/null && pwd )"

CAJETA_BIN="${CAJETA_BIN:-${REPO_ROOT}/build/src/cajeta}"

SRC_ROOT="${SCRIPT_DIR}/src"
BUILD_DIR="${SCRIPT_DIR}/build/cja"

if [[ ! -x "$CAJETA_BIN" ]]; then
    echo "error: cajeta compiler not found at $CAJETA_BIN" >&2
    echo "       build the compiler first: cd $REPO_ROOT && ./build.sh" >&2
    exit 1
fi

ENTRY_METHOD="helloworld.HelloWorld.run"

rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"

echo "Compiling cajeta sources → .cja archive (project-only)"
echo "  entry: $ENTRY_METHOD"
echo "  src:   $SRC_ROOT"
echo "  out:   $BUILD_DIR"
"$CAJETA_BIN" \
    --emit=cja \
    "$ENTRY_METHOD" \
    "$SRC_ROOT" \
    "$BUILD_DIR" \
    > "${BUILD_DIR}/cajeta-compile.log" 2>&1 || {
    echo "error: cajeta --emit=cja failed (see ${BUILD_DIR}/cajeta-compile.log)" >&2
    tail -20 "${BUILD_DIR}/cajeta-compile.log" >&2
    exit 1
}

OUT_CJA=$(find "$BUILD_DIR" -name '*.cja' | head -1)
if [[ -z "$OUT_CJA" ]]; then
    echo "error: no .cja file produced" >&2
    exit 1
fi

echo ""
echo "archive: $OUT_CJA"
ls -l "$OUT_CJA" | awk '{print "size:    " $5 " bytes"}'
echo "header:  $(xxd -l 8 -p "$OUT_CJA" | xxd -r -p)"
