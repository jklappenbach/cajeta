#!/usr/bin/env bash
# Build the Tour uber `.cja` archive, then run it.
#
# An uber archive bundles LLVM bitcode (user modules + the parsed stdlib, which
# already has the runtime linked in) and is built by the LLVM-23 cajeta-llvm
# fork — so we execute it by extracting that bitcode and linking it with the
# fork's `clang` (a stock clang can't read LLVM-23 bitcode). The uber form
# carries the compiler-synthesized C `main` dispatcher (it marshals argv into
# the entry's String[]), so no external entry is needed. -ffunction-sections +
# --gc-sections drop the stdlib code the Tour doesn't use (e.g. the OpenSSL-
# backed TLS paths), so no extra native libs are needed.
set -euo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
REPO_ROOT="$( cd -- "${SCRIPT_DIR}/../.." &> /dev/null && pwd )"

CAJETA_BIN="${CAJETA_BIN:-${REPO_ROOT}/build/src/cajeta}"
# LLVM-23 clang from the cajeta-llvm fork (reads the compiler's LLVM-23 bitcode).
CLANG_BIN="${CLANG_BIN:-${REPO_ROOT}/../cajeta-llvm/build-cajeta/bin/clang-23}"

# 1. Build the uber archive.
"${SCRIPT_DIR}/build-uber.sh"

CJA="$( find "${SCRIPT_DIR}/build/uber" -name '*.cja' | head -1 )"
if [[ -z "$CJA" ]]; then
    echo "error: no .cja produced by build-uber.sh" >&2
    exit 1
fi

if [[ ! -x "$CLANG_BIN" ]]; then
    echo "error: LLVM-23 clang not found at $CLANG_BIN" >&2
    echo "       set CLANG_BIN to the cajeta-llvm fork's clang-23" >&2
    exit 1
fi

# 2. Extract the bundled bitcode.
RUN_DIR="${SCRIPT_DIR}/build/uber-run"
rm -rf "$RUN_DIR"; mkdir -p "$RUN_DIR"
echo ""
echo "[run] extracting bitcode from $(basename "$CJA")"
"$CAJETA_BIN" archive extract "$CJA" -C "$RUN_DIR" > /dev/null

# 3. Link the bitcode + the C entry shim into a native binary.
OUT_BINARY="${RUN_DIR}/tour-uber"
BCS=()
while IFS= read -r -d '' f; do BCS+=("$f"); done \
    < <(find "$RUN_DIR" -name '*.bc' -print0)
echo "[run] linking ${#BCS[@]} bitcode modules → ${OUT_BINARY}"
"$CLANG_BIN" \
    -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -o "$OUT_BINARY" \
    "${BCS[@]}" \
    -lpthread -lm

# 4. Run.
echo ""
echo "=== running $OUT_BINARY (from the uber archive) ==="
exec "$OUT_BINARY" "$@"
