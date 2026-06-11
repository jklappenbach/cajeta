#!/usr/bin/env bash
# Build HelloWorld into an uber cajeta archive (.cja). Same container
# as the thin form, but the manifest's `kind` field is `"uber"` and
# (in a future session with --classpath ingestion) the archive will
# also bundle every transitively-referenced dependency archive.
#
# v1 uber-emit produces a thin-shaped archive with the uber kind tag —
# the transitive-dep bundling lands when --classpath comes online.
# Consumers can still identify it as uber-intent today.
#
# See docs/Compilation.md § Uber archives for the design.

set -euo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
REPO_ROOT="$( cd -- "${SCRIPT_DIR}/../.." &> /dev/null && pwd )"

CAJETA_BIN="${CAJETA_BIN:-${REPO_ROOT}/build/src/cajeta}"

SRC_ROOT="${SCRIPT_DIR}/src"
BUILD_DIR="${SCRIPT_DIR}/build/uber"

if [[ ! -x "$CAJETA_BIN" ]]; then
    echo "error: cajeta compiler not found at $CAJETA_BIN" >&2
    echo "       build the compiler first: cd $REPO_ROOT && ./build.sh" >&2
    exit 1
fi

ENTRY_METHOD="helloworld.HelloWorld.run"

rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"

echo "Compiling cajeta sources → .cja archive (uber)"
echo "  entry: $ENTRY_METHOD"
echo "  src:   $SRC_ROOT"
echo "  out:   $BUILD_DIR"
"$CAJETA_BIN" \
    --emit=uber \
    "$ENTRY_METHOD" \
    "$SRC_ROOT" \
    "$BUILD_DIR" \
    > "${BUILD_DIR}/cajeta-compile.log" 2>&1 || {
    echo "error: cajeta --emit=uber failed (see ${BUILD_DIR}/cajeta-compile.log)" >&2
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
