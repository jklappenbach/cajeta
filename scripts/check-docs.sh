#!/usr/bin/env bash
# Umbrella docs checker (docs-refactor plan 2.3.1): links, snippets,
# docstring examples — one command, aggregate exit.
set -uo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)"

rc=0
"$SCRIPT_DIR/check-doc-links.sh"           || rc=1
"$SCRIPT_DIR/check-doc-snippets.sh"        || rc=1
"$SCRIPT_DIR/check-docstring-examples.sh"  || rc=1

if [ "$rc" -ne 0 ]; then
    echo "check-docs: FAILURES (see above)"
else
    echo "check-docs: all checks OK"
fi
exit "$rc"
