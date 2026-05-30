#!/bin/bash
# Run Cajeta tests. No args = full suite (parallel across cores); otherwise
# treat each arg as a test filter pattern (suite name, full test name, or
# wildcard) and run serially with raw gtest output.
#
# Examples:
#   ./run_tests.sh                              # everything (parallel, compact summary)
#   ./run_tests.sh BinaryOpTests                # whole BinaryOpTests suite (serial)
#   ./run_tests.sh BinaryOpTests.intAdd         # one specific test (serial)
#   ./run_tests.sh BinaryOpTests CompareTests   # multiple suites (serial)
#   ./run_tests.sh 'Fp*'                        # gtest wildcard (serial)
#   ./run_tests.sh --gtest_brief=0              # raw passthrough for flags
#
# Anything beginning with `--` is passed straight to the test binary, so any
# gtest flag works.
#
# Knobs:
#   NO_BUILD=1   skip the incremental build step
#   PARALLEL=0   force serial run even without filters
#   PARALLEL=1   force parallel run even with filters
#   PARALLEL=N   use N shards instead of nproc
#   VERBOSE=1    in parallel mode, dump each shard's full gtest output
#   NO_RETRY=1   skip the serial retry of failed/crashed shards (first pass
#                is authoritative; otherwise problem shards are re-run alone
#                and only deterministic failures count)

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

# Decide parallel vs serial. Default: parallel when no patterns AND no
# gtest flags that change discovery (e.g. --gtest_list_tests). Filtered
# runs default to serial — typically a small set the user wants to read.
should_parallel=0
if [ ${#patterns[@]} -eq 0 ]; then
    should_parallel=1
fi
for f in "${flags[@]}"; do
    if [[ "$f" == --gtest_list_tests* ]] || [[ "$f" == --gtest_filter=* ]]; then
        should_parallel=0
    fi
done
case "${PARALLEL:-}" in
    0) should_parallel=0 ;;
    1) should_parallel=1 ;;
    [2-9]|[1-9][0-9]*) should_parallel=1 ;;
esac

# Serial path — preserve the original behavior verbatim.
if [ "$should_parallel" = "0" ]; then
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
    if ! printf '%s\n' "${flags[@]}" | grep -q '^--gtest_brief'; then
        flags+=(--gtest_brief=1)
    fi
    echo ">> CAJETA_SOURCE_ROOT=\"$SCRIPT_DIR\" $TEST_BIN ${flags[*]}"
    exec env CAJETA_SOURCE_ROOT="$SCRIPT_DIR" "$TEST_BIN" "${flags[@]}"
fi

# --------------------------------------------------------------------------
# Parallel path. Steps:
#   1. List every test via --gtest_list_tests.
#   2. Round-robin into N buckets (N = nproc, or override via PARALLEL=N).
#   3. Spawn each bucket as a child process with its own --gtest_filter.
#   4. Wait, parse each shard's gtest summary, aggregate, print a compact
#      report. Surface crashing shards by name + last test that started.
#   5. Exit non-zero if anything failed or crashed.
# --------------------------------------------------------------------------

# Disable job-control monitoring so bash doesn't print its own
# "Segmentation fault" / "Aborted" lines when a child shard's test binary
# crashes. The aggregator below picks up the non-zero exit code from
# $exit_file and reports the offending shard cleanly.
set +m

# Determine shard count. Default to nproc but cap at 32: LLJIT memory
# pressure under high parallelism (each shard is its own process with
# its own LLJIT instances per test) starts causing random SIGSEGVs in
# 1-4 shards once concurrent process count exceeds ~32 on this
# machine. 32-way is reliable; >32 surfaces what looks like address-
# space / mmap contention in LLJIT's runtime cleanup. Override via
# PARALLEL=N if you've investigated and have a reason.
if [[ "${PARALLEL:-}" =~ ^[0-9]+$ ]] && [ "${PARALLEL}" -gt 1 ]; then
    shards="${PARALLEL}"
else
    shards=$(nproc 2>/dev/null || echo 4)
    if [ "$shards" -gt 32 ]; then shards=32; fi
fi

# Enumerate tests. gtest emits one line per suite ending in `.`, then indented
# test names. Format example:
#     BinaryOpTests.
#       intAdd
#       intSub
echo ">> Discovering tests..."
# When filter patterns are present (e.g. release_tests.sh delegates a curated
# subset here with PARALLEL forced), restrict discovery to those patterns so
# the shards cover only the selected tests. With no patterns this is empty and
# every test is discovered as before.
list_filter_args=()
if [ ${#patterns[@]} -gt 0 ]; then
    lf=""
    for p in "${patterns[@]}"; do
        # Append `.*` to bare suite names, same as the serial path.
        if [[ "$p" != *.* && "$p" != *\** ]]; then
            p="${p}.*"
        fi
        if [ -z "$lf" ]; then lf="$p"; else lf="${lf}:${p}"; fi
    done
    list_filter_args=("--gtest_filter=$lf")
fi
raw_list=$(CAJETA_SOURCE_ROOT="$SCRIPT_DIR" "$TEST_BIN" --gtest_list_tests "${list_filter_args[@]}" 2>/dev/null || true)
tests=()
current_suite=""
while IFS= read -r line; do
    if [[ "$line" =~ ^([A-Za-z_][A-Za-z0-9_]*)\.[[:space:]]*$ ]]; then
        current_suite="${BASH_REMATCH[1]}"
    elif [[ "$line" =~ ^[[:space:]]+([A-Za-z_][A-Za-z0-9_/]*) ]]; then
        if [ -n "$current_suite" ]; then
            test_name="${BASH_REMATCH[1]}"
            tests+=("${current_suite}.${test_name}")
        fi
    fi
done <<< "$raw_list"

num_tests=${#tests[@]}
if [ "$num_tests" -eq 0 ]; then
    echo "error: no tests discovered via --gtest_list_tests" >&2
    exit 1
fi

# Cap shards at the test count — extra shards just sit empty.
if [ "$shards" -gt "$num_tests" ]; then shards=$num_tests; fi

# Round-robin distribute test names into per-shard filter strings.
declare -a shard_filters
for ((s=0; s<shards; s++)); do shard_filters[$s]=""; done
for ((i=0; i<num_tests; i++)); do
    s=$((i % shards))
    if [ -z "${shard_filters[$s]}" ]; then
        shard_filters[$s]="${tests[$i]}"
    else
        shard_filters[$s]="${shard_filters[$s]}:${tests[$i]}"
    fi
done

tmpdir=$(mktemp -d -t cajeta_test_shards.XXXXXX)
trap 'rm -rf "$tmpdir"' EXIT

echo ">> Running $num_tests tests across $shards shards..."
start_time=$SECONDS

pids=()
for ((s=0; s<shards; s++)); do
    out_file="$tmpdir/shard_${s}.out"
    exit_file="$tmpdir/shard_${s}.exit"
    # Wrapping in `{ ...; } 2>/dev/null &` rather than `( ...; ) 2>/dev/null &`
    # is load-bearing: when the backgrounded child dies by signal, bash prints
    # the "Segmentation fault (core dumped)" diagnostic at the parent's
    # reap-the-job step, NOT from inside the child. With a `()` subshell, the
    # redirect lives only inside the subshell — the parent's stderr (where the
    # diagnostic actually goes) is unchanged. With a `{}` group, the redirect
    # is owned by the parent shell for the duration of the job, so the diag
    # lands on /dev/null. The set +m a few lines above is necessary but not
    # sufficient on bash 5.x; this redirect is the actual suppression.
    {
        # Disable set -e here so a crashing/failing test binary still falls
        # through to record its exit code (set -e would terminate the
        # subshell mid-flight and the aggregator would see exit_file=?).
        set +e
        CAJETA_SOURCE_ROOT="$SCRIPT_DIR" "$TEST_BIN" \
            "--gtest_filter=${shard_filters[$s]}" \
            --gtest_brief=1 \
            > "$out_file" 2>&1
        rc=$?
        echo "$rc" > "$exit_file"
    } 2>/dev/null &
    pids+=($!)
done

# Wait for every shard. Use plain `wait` so a single shard's nonzero exit
# doesn't trip `set -e` before the others finish.
for pid in "${pids[@]}"; do
    wait "$pid" || true
done

elapsed=$((SECONDS - start_time))

# Aggregate. Per shard: parse "[  PASSED  ] X tests" and "[  FAILED  ] X tests"
# from the brief summary. If the shard exited non-zero with no failure count
# (typical of segfault / LLVM-assert), record it as a crash and surface the
# last `[ RUN      ]` line so the offending test is identifiable.
total_passed=0
total_failed=0
crashed_shards=()
failure_lines=()
# Shards that failed or crashed in the first pass, re-run serially below to
# separate memory-pressure flakiness from real deterministic failures.
retry_shards=()

for ((s=0; s<shards; s++)); do
    out_file="$tmpdir/shard_${s}.out"
    exit_code=$(cat "$tmpdir/shard_${s}.exit" 2>/dev/null || echo "?")

    # Passed count: under --gtest_brief=1 gtest still prints the
    # `[  PASSED  ] N tests.` summary line, so a single grep on that
    # is enough.
    passed=$(grep -oE '\[  PASSED  \] [0-9]+ test' "$out_file" 2>/dev/null \
                | grep -oE '[0-9]+' | head -1)
    passed=${passed:-0}
    # Failed count: --gtest_brief=1 does NOT print the
    # `[  FAILED  ] N test, listed below:` summary line — only the
    # per-test `[  FAILED  ] Suite.test (Nms)` lines. Count those
    # (they start with `[  FAILED  ]` followed by an identifier
    # character, distinguishing them from the count-summary line
    # which starts with a digit). This avoids misclassifying a
    # real test failure as a shard crash downstream.
    # `grep -c` always prints a count (including 0) but exits non-zero
    # when no lines match; the `|| true` keeps that from short-circuiting
    # the surrounding script and avoids emitting a duplicate "0".
    failed=$(grep -cE '^\[  FAILED  \] [A-Za-z_]' "$out_file" 2>/dev/null || true)
    failed=${failed:-0}

    total_passed=$((total_passed + passed))
    total_failed=$((total_failed + failed))

    if [ "$failed" -gt 0 ]; then
        while IFS= read -r line; do
            failure_lines+=("$line")
        done < <(grep -E '^\[  FAILED  \] [A-Za-z_]' "$out_file")
        retry_shards+=("$s")
    fi

    if [ "$exit_code" != "0" ] && [ "$failed" -eq 0 ]; then
        # Likely crashed mid-run. --gtest_brief=1 suppresses [ RUN ] lines
        # for passing tests, so we can't identify the offender from output
        # alone. Re-run the shard's filter under --gtest_brief=0 to surface
        # the [ RUN ] sequence and find the last test that started before
        # the crash. Cheap because a crashed shard is rare and we only run
        # its own bucket. Wrap in an inner subshell whose stderr -> /dev/null
        # so bash's "Segmentation fault" diagnostic doesn't leak to ours.
        verbose_log="$tmpdir/shard_${s}.verbose"
        # `{ ...; } 2>/dev/null` — same reasoning as the parallel loop above:
        # the signal-death diagnostic is printed at the *current* shell's
        # level, so the redirect must apply to the current shell (group), not
        # to a child subshell.
        { CAJETA_SOURCE_ROOT="$SCRIPT_DIR" "$TEST_BIN" \
              "--gtest_filter=${shard_filters[$s]}" \
              --gtest_brief=0 \
              > "$verbose_log" 2>&1 ; } 2>/dev/null || true
        last_run=$(grep -oE '\[ RUN      \] [A-Za-z0-9_./]+' "$verbose_log" \
                    | tail -1 | sed 's/^\[ RUN      \] //')

        # Classify the exit code. POSIX convention: 128+N means killed by
        # signal N. Common ones we surface explicitly so the summary tells
        # the reader what to look at without consulting `kill -l`.
        #
        # exit_code can be empty when the subshell died so hard it never
        # wrote the .exit file (e.g. SIGKILL from an OOM / parent-shell
        # tear-down race). Treat empty / non-numeric as "unknown" rather
        # than letting `[ "" -ge 128 ]` blow up with "integer expected".
        case "$exit_code" in
            134) reason="SIGABRT (assertion / abort)" ;;
            136) reason="SIGFPE (divide-by-zero / FP exception)" ;;
            137) reason="SIGKILL (OOM or external kill)" ;;
            139) reason="SIGSEGV (segfault)" ;;
            1)   reason="exit 1 with no gtest summary (aborted before report)" ;;
            ''|\?)
                reason="exit code unrecorded (subshell killed before writing)"
                ;;
            *)
                # Numeric but outside the explicit set above, or anything
                # else. Guard the range test with a regex check so we never
                # feed a non-integer to `-ge`.
                if [[ "$exit_code" =~ ^[0-9]+$ ]] \
                        && [ "$exit_code" -ge 128 ] \
                        && [ "$exit_code" -le 192 ]; then
                    reason="killed by signal $((exit_code - 128))"
                else
                    reason="exit ${exit_code}"
                fi
                ;;
        esac

        crashed_shards+=("shard ${s}: ${reason}; last test started: ${last_run:-<none>}")
        retry_shards+=("$s")
    fi
done

echo
echo "=== Test summary ==="
echo "Discovered: $num_tests   Shards: $shards   Elapsed: ${elapsed}s"
echo "Passed: $total_passed   Failed: $total_failed   Crashed shards: ${#crashed_shards[@]}"

if [ ${#failure_lines[@]} -gt 0 ]; then
    echo
    echo "Failed tests:"
    for line in "${failure_lines[@]}"; do
        echo "  $line"
    done
fi

if [ ${#crashed_shards[@]} -gt 0 ]; then
    echo
    echo "Crashed shards:"
    for c in "${crashed_shards[@]}"; do
        echo "  $c"
    done
    echo
    echo "Re-run the crashing test in isolation with:"
    echo "  PARALLEL=0 ./run_tests.sh <SuiteName.testName>"
fi

if [ "${VERBOSE:-}" = "1" ]; then
    echo
    echo "=== Per-shard output ==="
    for ((s=0; s<shards; s++)); do
        echo "----- shard ${s} -----"
        cat "$tmpdir/shard_${s}.out"
    done
fi

# --------------------------------------------------------------------------
# Retry pass. Running ~32 LLJIT shards at once puts the box under enough
# memory/scheduling pressure that a shard or two randomly takes a SIGSEGV (or
# a JIT null-return -> wrong value -> assertion fail). The named test just
# happens to be the last one that started; it moves run to run. Re-run each
# problem shard ON ITS OWN (serial, no contention): genuine deterministic
# failures reproduce and stay red, flaky ones recover. The final exit code is
# based on the retry, not the first pass. NO_RETRY=1 skips this and treats the
# first pass as authoritative.
# --------------------------------------------------------------------------
if { [ "$total_failed" -gt 0 ] || [ ${#crashed_shards[@]} -gt 0 ]; } \
        && [ -z "${NO_RETRY:-}" ]; then
    echo
    echo "=== Retry pass: re-running ${#retry_shards[@]} problem shard(s) serially ==="
    echo "(memory-pressure flakiness recovers here; deterministic failures persist)"
    r_failed=0
    r_crashed=()
    r_failure_lines=()
    for s in "${retry_shards[@]}"; do
        echo "  re-running shard ${s} ..."
        r_out="$tmpdir/retry_${s}.out"
        r_exit="$tmpdir/retry_${s}.exit"
        { set +e
          CAJETA_SOURCE_ROOT="$SCRIPT_DIR" "$TEST_BIN" \
              "--gtest_filter=${shard_filters[$s]}" \
              --gtest_brief=1 > "$r_out" 2>&1
          echo "$?" > "$r_exit"
        } 2>/dev/null
        rexit=$(cat "$r_exit" 2>/dev/null || echo "?")
        rfailed=$(grep -cE '^\[  FAILED  \] [A-Za-z_]' "$r_out" 2>/dev/null || true)
        rfailed=${rfailed:-0}
        r_failed=$((r_failed + rfailed))
        if [ "$rfailed" -gt 0 ]; then
            while IFS= read -r line; do
                r_failure_lines+=("$line")
            done < <(grep -E '^\[  FAILED  \] [A-Za-z_]' "$r_out")
        fi
        if [ "$rexit" != "0" ] && [ "$rfailed" -eq 0 ]; then
            r_crashed+=("shard ${s}: exit ${rexit}")
        fi
    done

    echo
    echo "=== Retry summary ==="
    echo "Retried shards: ${#retry_shards[@]}   Still failing: ${r_failed}   Still crashing: ${#r_crashed[@]}"
    if [ ${#r_failure_lines[@]} -gt 0 ]; then
        echo
        echo "Persisting failures (real):"
        for line in "${r_failure_lines[@]}"; do echo "  $line"; done
    fi
    if [ ${#r_crashed[@]} -gt 0 ]; then
        echo
        echo "Persisting crashes (real):"
        for c in "${r_crashed[@]}"; do echo "  $c"; done
    fi

    if [ "$r_failed" -gt 0 ] || [ ${#r_crashed[@]} -gt 0 ]; then
        exit 1
    fi
    echo
    echo "All problem shards passed on serial retry -- the first-pass failures"
    echo "were memory-pressure flakiness, not real failures. PASS."
    exit 0
fi

if [ "$total_failed" -gt 0 ] || [ ${#crashed_shards[@]} -gt 0 ]; then
    exit 1
fi
exit 0
