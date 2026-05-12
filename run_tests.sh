#!/bin/bash
# Run Cajeta tests. No args = full suite; otherwise treat each arg as a test
# filter pattern (suite name, full test name, or wildcard).
#
# Examples:
#   ./run_tests.sh                              # everything (brief output)
#   ./run_tests.sh BinaryOpTests                # whole BinaryOpTests suite
#   ./run_tests.sh BinaryOpTests.intAdd         # one specific test
#   ./run_tests.sh BinaryOpTests CompareTests   # multiple suites
#   ./run_tests.sh 'Fp*'                        # gtest wildcard
#   ./run_tests.sh --gtest_brief=0              # raw passthrough for flags
#
# Anything beginning with `--` is passed straight to the test binary, so any
# gtest flag works. Set NO_BUILD=1 to skip the incremental build step.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

TEST_BIN="build/test/cajeta_test"

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

# Split args: anything starting with `--` is a gtest flag, the rest are filter
# patterns we join with `:` and hand to --gtest_filter.
flags=()
patterns=()
for arg in "$@"; do
    case "$arg" in
        --*) flags+=("$arg") ;;
        *)   patterns+=("$arg") ;;
    esac
done

# Build the gtest filter from the positional patterns.
if [ ${#patterns[@]} -gt 0 ]; then
    filter=""
    for p in "${patterns[@]}"; do
        # Append `.*` to bare suite names so `BinaryOpTests` runs the whole suite.
        if [[ "$p" != *.* && "$p" != *\** ]]; then
            p="${p}.*"
        fi
        if [ -z "$filter" ]; then
            filter="$p"
        else
            filter="${filter}:${p}"
        fi
    done
    flags+=("--gtest_filter=$filter")
fi

# Default to brief output when the user didn't override.
if ! printf '%s\n' "${flags[@]}" | grep -q '^--gtest_brief'; then
    flags+=(--gtest_brief=1)
fi

echo ">> CAJETA_SOURCE_ROOT=\"$SCRIPT_DIR\" $TEST_BIN ${flags[*]}"
CAJETA_SOURCE_ROOT="$SCRIPT_DIR" "$TEST_BIN" "${flags[@]}"
