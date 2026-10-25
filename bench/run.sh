#!/usr/bin/env bash
# Build and run the profile benchmark harness with the cajeta build tool.
#
# Reads cajeta.json and runs its `run` task, which builds build/profile and then
# executes it. The build tool drives both steps — no hand-rolled compile/link/run.
#
# Defaults to the fork-LLVM compiler at build-cajeta/src/cajeta. Override with
# CAJETA=/path/to/cajeta. Extra args after `--` pass through to the harness
# (e.g. `./run.sh -- --list`).
set -euo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
REPO_ROOT="$( cd -- "${SCRIPT_DIR}/../.." &> /dev/null && pwd )"
CAJETA="${CAJETA:-${REPO_ROOT}/build-cajeta/src/cajeta}"

if [[ ! -x "$CAJETA" ]]; then
    echo "error: cajeta build tool not found at $CAJETA" >&2
    echo "       build the compiler first: cd $REPO_ROOT && ./build.sh" >&2
    echo "       or override with CAJETA=/path/to/cajeta" >&2
    exit 1
fi

cd "$SCRIPT_DIR"
exec "$CAJETA" run "$@"
