#!/bin/bash
# Demonstrate the buildtool surface against this sample.
set -e

CAJETA="${CAJETA:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.."; pwd)/build/src/cajeta}"
if [ ! -x "$CAJETA" ]; then
    echo "error: cajeta binary not found at $CAJETA" >&2
    echo "  override with CAJETA=/path/to/cajeta" >&2
    exit 1
fi
cd "$(dirname "${BASH_SOURCE[0]}")"

echo "═══ 1. List tasks ═══"
"$CAJETA" tasks
echo

echo "═══ 2. Resolved properties (stack-version + built-ins) ═══"
"$CAJETA" info --properties
echo

echo "═══ 3. Render the 'build' task structure (without running it) ═══"
"$CAJETA" task build --show
echo

echo "═══ 4. Render 'release' with substitutions applied ═══"
"$CAJETA" task release --show
echo

echo "═══ 5. Write the lockfile from the current resolved state ═══"
"$CAJETA" info --write-lockfile
echo

if [ -f cajeta.lock ]; then
    echo "═══ 6. cajeta.lock contents ═══"
    cat cajeta.lock
fi
