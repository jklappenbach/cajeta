#!/usr/bin/env bash
# Light sweep — the between-phase gate. ~90 seconds on the 32-core host.
#
# Runs the curated subset in test/light_filter.txt: one representative per code
# path, chosen for breadth-per-second. Use it after every step of a plan, so a
# broad regression is caught while the change that caused it is still one commit
# ago instead of five phases back.
#
#   ./light_tests.sh                 # the gate (190 tests, ~90s)
#   ./light_tests.sh --math          # + test/light_filter_math.txt (~95s @32w)
#   ./light_tests.sh --list          # print the resolved test list, run nothing
#   ./light_tests.sh --dry-run       # validate patterns + print the plan, run nothing
#   ./light_tests.sh shard=16        # fewer workers (see the cost note below)
#
# THIS IS NOT A MERGE GATE. It is a fast smoke signal. Merging still requires
# the full sweep (./cajeta_tests.sh) and, for a release, ./release_tests.sh.
#
# COST NOTE (measured, not estimated)
# -----------------------------------
# 143s wall for 190 tests on 32 workers with NO_BUILD=1.
#
# The cost is dominated by PROCESS COUNT, not test duration. BATCH=1 runs one
# process per suite, and every process that compiles Cajeta pays a ~16s stdlib
# prime regardless of what it asserts (measured: BinaryOpTests.intAdd 16.0s,
# DropGapTests.declareThenMoveAssign 16.2s despite a 0ms seed row; pure-C++
# suites are 0.01s). This list spans 96 JIT-touching suites = ~1536 CPU-seconds
# of prime, ~48s of the wall, before any assertion runs.
#
# So: to speed this tier up, reduce the number of distinct JIT SUITES it
# touches. Dropping individual cheap tests from suites already listed saves
# nothing at all.
#
# Execution is delegated to cajeta_tests.sh (PARALLEL forced), so this inherits
# its crash isolation, per-test timeouts, suite batching, and summary format.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR" || exit 2

FILTER_FILE="$SCRIPT_DIR/test/light_filter.txt"
MATH_FILE="$SCRIPT_DIR/test/light_filter_math.txt"

WITH_MATH=0
LIST_ONLY=0
DRY_RUN=0
passthru=()
for arg in "$@"; do
    case "$arg" in
        --math)     WITH_MATH=1 ;;
        --list)     LIST_ONLY=1 ;;
        --dry-run)  DRY_RUN=1 ;;
        -h|--help)  sed -n '2,32p' "$0"; exit 0 ;;
        *)          passthru+=("$arg") ;;
    esac
done

[ -f "$FILTER_FILE" ] || { echo "error: missing $FILTER_FILE" >&2; exit 2; }

# ---- Read the pattern files -------------------------------------------------
# One fully-qualified Suite.test per line; `#` comments and blanks ignored.
read_patterns() {
    # shellcheck disable=SC2001
    sed -e 's/#.*//' -e 's/[[:space:]]*$//' -e '/^$/d' "$1"
}

patterns=()
while IFS= read -r p; do [ -n "$p" ] && patterns+=("$p"); done < <(read_patterns "$FILTER_FILE")

if [ "$WITH_MATH" = "1" ]; then
    [ -f "$MATH_FILE" ] || { echo "error: --math given but missing $MATH_FILE" >&2; exit 2; }
    while IFS= read -r p; do [ -n "$p" ] && patterns+=("$p"); done < <(read_patterns "$MATH_FILE")
fi

if [ ${#patterns[@]} -eq 0 ]; then
    echo "error: no patterns parsed from $FILTER_FILE" >&2
    exit 2
fi

if [ "$LIST_ONLY" = "1" ]; then
    printf '%s\n' "${patterns[@]}"
    exit 0
fi

# ---- Locate the test binary -------------------------------------------------
if   [ -x ./build/test/cajeta_test.exe ]; then TEST_BIN=./build/test/cajeta_test.exe
elif [ -x ./build/test/cajeta_test ];     then TEST_BIN=./build/test/cajeta_test
else
    echo "error: test binary not found under ./build/test — build first:" >&2
    echo "       cmake --build build --target cajeta_test" >&2
    exit 2
fi

# ---- Validate every pattern resolves ---------------------------------------
# The whole value of a curated gate is that it does not silently shrink. A
# renamed or deleted test must fail the run LOUDLY, not quietly reduce coverage.
# Mirrors release_tests.sh's zero-match check.
joined=""
for p in "${patterns[@]}"; do
    if [ -z "$joined" ]; then joined="$p"; else joined="${joined}:${p}"; fi
done

discovered="$(CAJETA_SOURCE_ROOT="$SCRIPT_DIR" "$TEST_BIN" \
    --gtest_list_tests --gtest_filter="$joined" 2>/dev/null \
  | awk '/^[A-Za-z_][A-Za-z0-9_]*\.[[:space:]]*$/ { s=$1; next }
         /^[[:space:]]+[A-Za-z_]/ { print s $1 }')"

missing=()
for p in "${patterns[@]}"; do
    if ! grep -qxF "$p" <<< "$discovered"; then missing+=("$p"); fi
done

if [ ${#missing[@]} -gt 0 ]; then
    echo "error: ${#missing[@]} pattern(s) in the light filter match no test." >&2
    echo "       The gate would silently run reduced coverage. Fix the filter file." >&2
    printf '         %s\n' "${missing[@]}" >&2
    exit 2
fi

n_sel="$(grep -c . <<< "$discovered")"
echo ">> light sweep: ${#patterns[@]} patterns -> $n_sel tests$( [ "$WITH_MATH" = 1 ] && echo ' (incl. --math)')"

if [ "$DRY_RUN" = "1" ]; then
    echo ">> dry run: all patterns resolve; nothing executed."
    exit 0
fi

# ---- Delegate to the real runner -------------------------------------------
# Positional patterns + PARALLEL=1 is the same seam release_tests.sh uses:
# cajeta_tests.sh restricts discovery to the patterns, then shards them.
exec env PARALLEL="${PARALLEL:-1}" "$SCRIPT_DIR/cajeta_tests.sh" \
    "${patterns[@]}" "${passthru[@]+"${passthru[@]}"}"
