#!/usr/bin/env bash
# Crash-isolated parallel regression sweep.
#
# Runs each gtest suite in cajeta_test in its OWN process, so a segfault/abort
# in one suite can't truncate or bogus-tally the rest (a single-process full
# run dies at the first crasher and reports garbage downstream). Suites are
# fanned out across all cores; results are aggregated into a pass/fail/crash
# summary that names the suites needing attention.
#
# Usage:
#   ./regression_tests.sh [--manifest FILE] [jobs] [path-to-cajeta_test]
#     --manifest FILE     run ONLY the fully-qualified `Suite.test` names listed
#                         in FILE (also settable as REGRESSION_MANIFEST=FILE).
#                         Names are grouped by suite so crash isolation is kept.
#                         Exits 2 if any listed name no longer resolves.
#                         See test/regression_filter.txt — the bug-pinning set.
#     jobs                concurrent workers              (default: nproc)
#     path-to-cajeta_test test binary                     (default: ./build/test/cajeta_test[.exe])
#
# Examples:
#   ./regression_tests.sh                                        # every suite
#   ./regression_tests.sh --manifest test/regression_filter.txt  # bug-pinning set
#   grep -A99 '^# --- ir' test/regression_filter.txt | grep '^[A-Za-z]' > /tmp/m.txt
#   ./regression_tests.sh --manifest /tmp/m.txt                  # one section
#
# Exit code: 0 if every suite is clean; 1 if any failures or crashes.
set -u
export PATH="/c/msys64/mingw64/bin:$PATH"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT" || exit 2

# Optional `--manifest FILE` (or REGRESSION_MANIFEST=FILE) restricts the sweep to
# the fully-qualified `Suite.test` names listed in FILE. Consumed before the
# positional args so `jobs` / `path-to-cajeta_test` keep working either way.
MANIFEST="${REGRESSION_MANIFEST:-}"
if [ "${1:-}" = "--manifest" ]; then
    MANIFEST="${2:-}"
    [ -n "$MANIFEST" ] || { echo "error: --manifest needs a file argument" >&2; exit 2; }
    shift 2
fi

JOBS="${1:-$(nproc 2>/dev/null || echo 4)}"
EXE="${2:-}"
if [ -z "$EXE" ]; then
    if   [ -x ./build/test/cajeta_test.exe ]; then EXE=./build/test/cajeta_test.exe
    else EXE=./build/test/cajeta_test; fi
fi
if [ ! -x "$EXE" ]; then
    echo "error: test binary not found/executable: $EXE" >&2
    echo "       build first (cmake --build build) or pass the path as arg 2." >&2
    exit 2
fi

OUT="${TMPDIR:-/tmp}/cajeta_sweep"
RES="$OUT/res"
LOGS="$OUT/logs"
rm -rf "$OUT"; mkdir -p "$RES" "$LOGS"

FILTERS="$OUT/filters"
mkdir -p "$FILTERS"

if [ -n "$MANIFEST" ]; then
    [ -f "$MANIFEST" ] || { echo "error: manifest not found: $MANIFEST" >&2; exit 2; }
    echo "reading manifest: $MANIFEST"
    # Strip comments/blanks, then group the fully-qualified names by suite so we
    # keep one-process-per-suite crash isolation while running ONLY the listed
    # tests (running "$S.*" here would silently pull in the suite's other tests).
    sed -e 's/#.*//' -e 's/[[:space:]]*$//' -e '/^$/d' "$MANIFEST" \
        | sort -u > "$OUT/manifest.txt"
    awk -F. '{ s=$1; rest=substr($0, length(s)+2);
               if (rest == "") next;
               f[s] = (s in f) ? f[s] ":" $0 : $0 }
             END { for (s in f) print s "\t" f[s] }' "$OUT/manifest.txt" \
        > "$OUT/suite_filters.tsv"
    cut -f1 "$OUT/suite_filters.tsv" | sort -u > "$OUT/suites.txt"
    while IFS=$'\t' read -r s expr; do printf '%s' "$expr" > "$FILTERS/$s"; done \
        < "$OUT/suite_filters.tsv"

    # Fail loudly if any listed name no longer resolves — a rename must not
    # silently shrink the gate.
    WANT=$(wc -l < "$OUT/manifest.txt" | tr -d ' ')
    GOT=$("$EXE" --gtest_list_tests --gtest_filter="$(paste -sd: - < "$OUT/manifest.txt")" \
            2>/dev/null | grep -cE '^  ')
    if [ "$WANT" != "$GOT" ]; then
        echo "error: manifest lists $WANT tests but only $GOT resolve in $EXE." >&2
        echo "       A test was renamed or removed; reconcile $MANIFEST." >&2
        exit 2
    fi
    N=$(wc -l < "$OUT/suites.txt" | tr -d ' ')
    echo "fanning $WANT tests across $N suites / $JOBS workers (binary: $EXE)"
else
    echo "listing suites..."
    "$EXE" --gtest_list_tests 2>/dev/null \
        | grep -E '^[A-Za-z].*\.$' | sed 's/\.$//' | sort -u > "$OUT/suites.txt"
    N=$(wc -l < "$OUT/suites.txt" | tr -d ' ')
    echo "fanning $N suites across $JOBS workers (binary: $EXE)"
fi
echo ""

# One suite per process; own log + own result file (no shared-file contention).
# Config travels via exported env vars (not positional args) so the xargs
# worker shell — which inherits `set -u` because Git-bash exports SHELLOPTS —
# never trips over an unbound positional.
# rc: 0 = all passed, 1 = normal test failures, >1 = crash (139 SIGSEGV / 134
# SIGABRT / 132 SIGILL on this toolchain).
export SWEEP_EXE="$EXE" SWEEP_LOGS="$LOGS" SWEEP_RES="$RES" SWEEP_FILTERS="$FILTERS"
run_one() {
    local S="$1"
    [ -n "$S" ] || return 0
    local LOG="$SWEEP_LOGS/$S.log"
    # With a manifest, run only that suite's listed tests; otherwise the whole suite.
    local FILTER="$S.*"
    [ -f "$SWEEP_FILTERS/$S" ] && FILTER=$(cat "$SWEEP_FILTERS/$S")
    "$SWEEP_EXE" --gtest_filter="$FILTER" > "$LOG" 2>&1
    local RC=$?
    local PASS FAILED
    PASS=$(grep -cE '^\[       OK \]' "$LOG")
    # gtest in non-brief mode prints each failure TWICE (inline result with a
    # "(N ms)" timing suffix, plus a bare summary listing). Count only the
    # timed inline line so each failed test is tallied exactly once.
    FAILED=$(grep -cE '^\[  FAILED  \] .*\([0-9]+ ms\)$' "$LOG")
    printf '%s\t%s\t%s\t%s\n' "$S" "$RC" "$PASS" "$FAILED" > "$SWEEP_RES/$S.tsv"
}
export -f run_one

xargs -a "$OUT/suites.txt" -P "$JOBS" -I{} bash -c 'run_one "$@"' _ {}

cat "$RES"/*.tsv 2>/dev/null | sort > "$OUT/summary.tsv"

awk -F'\t' '
    { suites++; pass+=$3; fail+=$4;
      if      ($2==0) ok++;
      else if ($2==1) { failed_suites++; faillist[$1]=$4 }
      else            { crashed++; crash[$1]=$2; if ($4>0) faillist[$1]=$4 }
    }
    END {
      print  "================= REGRESSION SWEEP =================";
      printf "suites: %d      tests passed: %d      tests failed: %d\n", suites, pass, fail;
      printf "clean: %d     suites w/ failures: %d     crashed processes: %d\n", ok, failed_suites, crashed;
      if (length(faillist)) { print "\n-- suites with failing tests --";
          for (s in faillist) printf "  %-46s %d failed\n", s, faillist[s] }
      if (length(crash)) { print "\n-- crashed processes (rc: 139=SIGSEGV 134=abort) --";
          for (s in crash) printf "  %-46s rc=%d\n", s, crash[s] }
      print  "===================================================";
    }
' "$OUT/summary.tsv"

echo ""
echo "per-suite logs: $LOGS"
echo "summary tsv:    $OUT/summary.tsv  (cols: suite  rc  passed  failed)"

# Non-zero exit if anything failed or crashed.
awk -F'\t' 'BEGIN{bad=0} { if ($2!=0) bad=1 } END{ exit bad }' "$OUT/summary.tsv"
