#!/bin/bash
# Run the RELEASE regression subset — the curated set of cross-compilation-
# sensitive test suites listed in test/release_filter.txt, rather than the
# full ~1725-test battery (`run_tests.sh` with no args). This is what the
# release workflow runs on every target: the full suite is too expensive to
# run on all four matrix platforms, and most of it (the host-independent
# front-end suites) can't regress due to cross-compilation anyway. See
# test/release_filter.txt for the suite list and the rationale.
#
# The actual sharded execution + crash-isolation reporting is reused from
# run_tests.sh (this script just selects the subset and delegates), so a
# JIT crash on a non-x86 target is surfaced by name — exactly the kind of
# platform-specific lowering failure this subset exists to catch.
#
# Usage:
#   ./release_tests.sh              # build (incremental) then run the subset
#   NO_BUILD=1 ./release_tests.sh   # run against an already-built tree (CI)
#
# Knobs:
#   NO_BUILD=1   skip the incremental build step (CI sets this — the build
#                already happened in the workflow's dedicated Build step).
#   PARALLEL=N   shard count (default: nproc, capped at 32 by run_tests.sh).
#                PARALLEL=0 forces a serial run.
#
# Exit status is run_tests.sh's: non-zero if any selected test fails or a
# shard crashes, so this works directly as a release gate.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

TEST_BIN="build/test/cajeta_test"
FILTER_FILE="test/release_filter.txt"

if [ ! -f "$FILTER_FILE" ]; then
    echo "error: $FILTER_FILE not found" >&2
    exit 1
fi

if [ ! -f "build/build.ninja" ]; then
    echo ">> No build/ found, running ./setup.sh"
    ./setup.sh
fi

if [ -z "${NO_BUILD:-}" ]; then
    ./build.sh
fi

if [ ! -x "$TEST_BIN" ]; then
    echo "error: $TEST_BIN not built" >&2
    exit 1
fi

# Parse the filter file: drop `#` comments and blank lines, trim whitespace,
# collect one pattern per remaining line.
patterns=()
while IFS= read -r line || [ -n "$line" ]; do
    line="${line%%#*}"                          # strip trailing/whole-line comment
    line="${line#"${line%%[![:space:]]*}"}"     # ltrim
    line="${line%"${line##*[![:space:]]}"}"     # rtrim
    [ -z "$line" ] && continue
    patterns+=("$line")
done < "$FILTER_FILE"

if [ ${#patterns[@]} -eq 0 ]; then
    echo "error: $FILTER_FILE lists no patterns" >&2
    exit 1
fi

# Drift guard. gtest treats a --gtest_filter that matches zero tests as a
# success (0 run) — so a renamed/removed suite would silently drop coverage
# with a green checkmark. Enumerate the discovered suites once and fail if
# any pattern matches nothing.
discovered="$(CAJETA_SOURCE_ROOT="$SCRIPT_DIR" "$TEST_BIN" --gtest_list_tests 2>/dev/null || true)"
if [ -z "$discovered" ]; then
    echo "error: could not enumerate tests via --gtest_list_tests" >&2
    exit 1
fi
missing=()
for p in "${patterns[@]}"; do
    suite="${p%%.*}"
    # gtest prints each suite as a `SuiteName.` header at column 0.
    if ! grep -qE "^${suite}\." <<< "$discovered"; then
        missing+=("$p")
    fi
done
if [ ${#missing[@]} -gt 0 ]; then
    echo "error: release filter references suites with no matching tests:" >&2
    printf '  %s\n' "${missing[@]}" >&2
    echo "A suite was renamed or removed — update $FILTER_FILE." >&2
    exit 1
fi

# Delegate to run_tests.sh for sharded execution. PARALLEL is forced (default
# nproc) so the subset runs in parallel even though it's a filtered run —
# run_tests.sh's parallel discovery honors these patterns. NO_BUILD avoids a
# redundant second build (we already built / verified above).
echo ">> Release subset: ${#patterns[@]} suites (delegating to run_tests.sh, parallel)"
exec env NO_BUILD=1 PARALLEL="${PARALLEL:-$(nproc 2>/dev/null || echo 4)}" \
    ./run_tests.sh "${patterns[@]}"
