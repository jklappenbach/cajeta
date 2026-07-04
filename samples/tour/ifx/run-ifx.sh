#!/usr/bin/env bash
# Build + run the Cajeta ifx tour.
#
# Headless-safe: the demo binds the always-viable Null backend floor via
# BackendRegistry.instance().selectWindow(true), so it needs no display
# server, no GPU, no audio device. Exit code is the tour's self-check result.
set -euo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
REPO_ROOT="$( cd -- "${SCRIPT_DIR}/../../.." &> /dev/null && pwd )"
CAJETA="${CAJETA:-${REPO_ROOT}/build/src/cajeta}"

if [[ ! -x "$CAJETA" ]]; then
    echo "error: cajeta compiler not found at $CAJETA" >&2
    echo "       build the compiler first: cd $REPO_ROOT && ./build.sh" >&2
    echo "       or override with CAJETA=/path/to/cajeta" >&2
    exit 1
fi

cd "$SCRIPT_DIR"
"$CAJETA" build
exec ./build/ifx-tour
