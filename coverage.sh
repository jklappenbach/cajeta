#!/usr/bin/env bash
# C++ coverage for the cajeta compiler — measure, then USE the measurement to
# pare the test battery down.
#
# The aggregate percentage is the least interesting output. The point is
# PER-UNIT attribution: which suite (or test) covers which lines. With that you
# can answer the three questions that actually shrink a 1600-CPU-minute sweep:
#
#   1. Which units are REDUNDANT — they cover nothing another unit doesn't?
#   2. What is the MINIMAL subset that holds ~all of the coverage? (set cover)
#   3. Which code is covered by NOTHING — where do new tests need writing?
#
# Usage:
#   ./coverage.sh build                     configure + build the instrumented tree
#   ./coverage.sh run [--per=suite|test] [gtest_filter]
#                                           run units, one gcda tree each
#   ./coverage.sh report                    per-file summary, worst-covered first
#   ./coverage.sh optimize                  redundancy + greedy minimal subset
#   ./coverage.sh gaps                      files/functions covered by nothing
#   ./coverage.sh all                       build, run, report, optimize, gaps
#
# Env:
#   COV_BUILD=build-cov     instrumented build dir (NEVER the primary build/)
#   COV_DATA=.coverage      per-unit gcda trees + derived json
#   COV_JOBS=<nproc>        parallel units
#
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
COV_BUILD="${COV_BUILD:-$ROOT/build-cov}"
COV_DATA="${COV_DATA:-$ROOT/.coverage}"
COV_JOBS="${COV_JOBS:-$(nproc 2>/dev/null || echo 8)}"
TEST_BIN="$COV_BUILD/test/cajeta_test"

die() { echo "coverage: $*" >&2; exit 1; }

cmd_build() {
    # A SEPARATE tree. Instrumented objects must never be linked into the
    # binary the sweep or a release uses.
    cmake -S "$ROOT" -B "$COV_BUILD" -G Ninja \
          -DCMAKE_BUILD_TYPE=Debug -DCAJETA_COVERAGE=ON \
        || die "configure failed"
    cmake --build "$COV_BUILD" --target cajeta_test -j "$COV_JOBS" \
        || die "build failed"
    echo ">> instrumented build ready: $TEST_BIN"
}

# Run each unit in its own process with its own GCOV_PREFIX, so the .gcda tree
# it writes is attributable to that unit alone. This is the whole mechanism:
# a shared gcda tree accumulates and tells you nothing about who covered what.
cmd_run() {
    local per="suite" filter=""
    for a in "$@"; do
        case "$a" in
            --per=*) per="${a#--per=}" ;;
            *) filter="$a" ;;
        esac
    done
    [ -x "$TEST_BIN" ] || die "no instrumented binary — run './coverage.sh build'"

    rm -rf "$COV_DATA/units"; mkdir -p "$COV_DATA/units"

    # Enumerate units at the requested granularity.
    local units
    if [ "$per" = "test" ]; then
        units=$("$TEST_BIN" --gtest_list_tests ${filter:+--gtest_filter="$filter"} 2>/dev/null \
            | awk '/^[^ ]/{s=$1} /^  /{gsub(/ .*/,"",$1); print s $1}')
    else
        units=$("$TEST_BIN" --gtest_list_tests ${filter:+--gtest_filter="$filter"} 2>/dev/null \
            | awk '/^[^ ]/{sub(/\.$/,"",$1); print $1}')
    fi
    [ -n "$units" ] || die "no tests matched"

    local total; total=$(echo "$units" | wc -l)
    echo ">> $total units (--per=$per), $COV_JOBS at a time"

    # GCOV_PREFIX redirects .gcda writes into a per-unit tree. GCOV_PREFIX_STRIP
    # drops the absolute build path components so the trees stay shallow.
    local strip; strip=$(echo "$COV_BUILD" | tr -cd '/' | wc -c)
    # Suite units need the trailing * (Suite.* matches its tests); a TEST unit
    # must match exactly — `Suite.testFoo*` would also run testFooBar and
    # smear both tests' coverage into one tree.
    local glob="*"
    [ "$per" = "test" ] && glob=""
    echo "$units" | xargs -P "$COV_JOBS" -I{} bash -c '
        u="{}"; safe="${u//\//_}"; safe="${safe//:/_}"
        out="'"$COV_DATA"'/units/$safe"
        mkdir -p "$out"
        GCOV_PREFIX="$out" GCOV_PREFIX_STRIP='"$strip"' \
          "'"$TEST_BIN"'" --gtest_filter="$u'"$glob"'" >"$out/stdout.txt" 2>&1
        echo "$?" > "$out/rc"
    '
    echo ">> raw coverage in $COV_DATA/units"
}

# Convert each unit's gcda tree into a compact {file: [covered lines]} set.
# gcov --json-format is why neither lcov nor gcovr is needed.
cmd_index() {
    [ -d "$COV_DATA/units" ] || die "nothing to index — run './coverage.sh run'"
    python3 "$ROOT/tools/coverage/index_gcov.py" \
        --build "$COV_BUILD" --data "$COV_DATA" --jobs "$COV_JOBS" \
        || die "indexing failed"
}

cmd_report()   { cmd_index; python3 "$ROOT/tools/coverage/analyze.py" report   --data "$COV_DATA"; }
cmd_optimize() { cmd_index; python3 "$ROOT/tools/coverage/analyze.py" optimize --data "$COV_DATA"; }
cmd_gaps()     { cmd_index; python3 "$ROOT/tools/coverage/analyze.py" gaps     --data "$COV_DATA" --root "$ROOT"; }

case "${1:-}" in
    build)    shift; cmd_build "$@" ;;
    run)      shift; cmd_run "$@" ;;
    index)    shift; cmd_index "$@" ;;
    report)   shift; cmd_report "$@" ;;
    optimize) shift; cmd_optimize "$@" ;;
    gaps)     shift; cmd_gaps "$@" ;;
    all)      cmd_build && cmd_run && cmd_report && cmd_optimize && cmd_gaps ;;
    *)        sed -n '2,30p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//' ;;
esac
