#!/bin/bash
set -e
CAJETA="${CAJETA:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.."; pwd)/build/src/cajeta}"
if [ ! -x "$CAJETA" ]; then
    echo "error: cajeta binary not found at $CAJETA" >&2
    exit 1
fi
cd "$(dirname "${BASH_SOURCE[0]}")"

echo "═══ Tasks ═══"
"$CAJETA" tasks
echo

echo "═══ build (default — server binary) ═══"
"$CAJETA" task build --show
echo

echo "═══ build-migrate ═══"
"$CAJETA" task build-migrate --show
echo

echo "═══ build-all (parallel) ═══"
"$CAJETA" task build-all --show
