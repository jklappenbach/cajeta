#!/usr/bin/env bash
# Build and profile the kernel sample, writing cajeta.pftrace beside this file.
#
#     ./run.sh          # GPU build (amdgpu/gfx1151), the point of the sample
#     ./run.sh cpu      # portable build — host lanes only, no device tracks
#
# The binary prints the backend it actually got. A GPU build that fell back to
# the CPU produces a trace with no device tracks, and the printed line is how
# you tell that apart from a profiler problem.
set -euo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
REPO_ROOT="$( cd -- "${SCRIPT_DIR}/../.." &> /dev/null && pwd )"
CAJETA="${CAJETA:-${REPO_ROOT}/build/src/cajeta}"

TASK="${1:-gpu}"
case "$TASK" in
    gpu) BIN="build/kernel-profile-gpu" ;;
    cpu) BIN="build/kernel-profile-cpu" ;;
    *)   echo "usage: $0 [gpu|cpu]" >&2; exit 2 ;;
esac

if [[ ! -x "$CAJETA" ]]; then
    echo "error: cajeta not found at $CAJETA" >&2
    echo "       build the compiler first, or set CAJETA=/path/to/cajeta" >&2
    exit 1
fi

cd "$SCRIPT_DIR"

"$CAJETA" "$TASK"

# CAJETA_PROFILER=1 arms the sampler; the trace lands in the working directory
# unless CAJETA_PROFILER_OUT says otherwise.
CAJETA_PROFILER=1 "./$BIN"
