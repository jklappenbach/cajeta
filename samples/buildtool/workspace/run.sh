#!/bin/bash
# Walk the workspace: workspace root + each member.
set -e

CAJETA="${CAJETA:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.."; pwd)/build/src/cajeta}"
if [ ! -x "$CAJETA" ]; then
    echo "error: cajeta binary not found at $CAJETA" >&2
    exit 1
fi
cd "$(dirname "${BASH_SOURCE[0]}")"

echo "═══ Workspace root: tasks + structure ═══"
"$CAJETA" tasks
echo
"$CAJETA" task build --show
echo

for member in shared/core shared/util apps/api apps/cli; do
    echo "═══ Member: $member ═══"
    (cd "$member" && "$CAJETA" tasks)
    echo
    (cd "$member" && "$CAJETA" task build --show)
    echo
done
