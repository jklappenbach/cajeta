#!/usr/bin/env bash
# Build cvm with the cajeta build tool.
#
# Reads cajeta.json and runs its `build` task → build/cvm (a native binary,
# --emit=exe). This is the whole build: no hand-rolled compile/link steps —
# the build tool drives the compiler. Override the compiler with
# CAJETA=/path/to/cajeta.
set -euo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
REPO_ROOT="$( cd -- "${SCRIPT_DIR}/../.." &> /dev/null && pwd )"
CAJETA="${CAJETA:-${REPO_ROOT}/build/src/cajeta}"

if [[ ! -x "$CAJETA" ]]; then
    echo "error: cajeta build tool not found at $CAJETA" >&2
    echo "       build the compiler first: cd $REPO_ROOT && ./build.sh" >&2
    echo "       or override with CAJETA=/path/to/cajeta" >&2
    exit 1
fi

cd "$SCRIPT_DIR"
exec "$CAJETA" build "$@"
