#!/usr/bin/env bash
# Docstring example lint (docs-refactor plan 2.2.3, spec 6.4 guard).
#
# Rejects the invalid `#Type local = ...` pattern in doc-comment
# example code: the `#` transfer sigil belongs on return types,
# parameters, and move expressions — never on the receiving local.
# Anchored to declaration-start positions (line start, optionally
# after a comment marker) so valid lambda return types like
# `(X) -> #Y h = ...` don't match.
#
# Usage: check-docstring-examples.sh [path ...]
#   Default roots: runtime/src samples docs (repo-relative).
# Exit 0 = clean; 1 = violations (listed).
set -uo pipefail

REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." &>/dev/null && pwd)"

roots=("$@")
if [ ${#roots[@]} -eq 0 ]; then
    cd "$REPO_ROOT"
    roots=(runtime/src samples docs)
fi

hits="$(grep -rnE '^[[:space:]]*(\*|//)?[[:space:]]*#[A-Za-z0-9_]+(\[\])?[[:space:]]+[a-zA-Z_][A-Za-z0-9_]*[[:space:]]*=' \
    --include='*.cajeta' "${roots[@]}" 2>/dev/null)"

if [ -n "$hits" ]; then
    echo "$hits"
    echo "check-docstring-examples: invalid '#Type local =' example(s) found"
    exit 1
fi
echo "check-docstring-examples: OK"
exit 0
