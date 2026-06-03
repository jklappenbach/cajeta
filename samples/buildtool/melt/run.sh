#!/bin/bash
set -e
CAJETA="${CAJETA:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.."; pwd)/build/src/cajeta}"
if [ ! -x "$CAJETA" ]; then
    echo "error: cajeta binary not found at $CAJETA" >&2
    exit 1
fi
cd "$(dirname "${BASH_SOURCE[0]}")"

echo "═══ Melt details + block summary ═══"
"$CAJETA" info
echo

echo "(A melt has no tasks block. Trying 'cajeta tasks' below"
echo " should show an empty task list.)"
echo
echo "═══ cajeta tasks (expect empty) ═══"
"$CAJETA" tasks || true
