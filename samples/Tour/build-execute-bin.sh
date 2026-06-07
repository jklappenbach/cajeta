#!/usr/bin/env bash
# Build the Tour to a native binary and run it.
# Thin wrapper: build-bin.sh (compile --emit=obj + link) then exec the binary.
set -euo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"

"${SCRIPT_DIR}/build-bin.sh"

echo ""
echo "=== running ${SCRIPT_DIR}/build/tour ==="
exec "${SCRIPT_DIR}/build/tour" "$@"
