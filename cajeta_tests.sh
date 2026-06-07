#!/bin/bash
# Run Cajeta tests. No args = full suite (parallel across cores); otherwise
# treat each arg as a test filter pattern (suite name, full test name, or
# wildcard) and run serially with raw gtest output.
#
# Examples:
#   ./cajeta_tests.sh                              # everything (parallel, compact summary)
#   ./cajeta_tests.sh BinaryOpTests                # whole BinaryOpTests suite (serial)
#   ./cajeta_tests.sh BinaryOpTests.intAdd         # one specific test (serial)
#   ./cajeta_tests.sh BinaryOpTests CompareTests   # multiple suites (serial)
#   ./cajeta_tests.sh 'Fp*'                        # gtest wildcard (serial)
#   ./cajeta_tests.sh --gtest_brief=0              # raw passthrough for flags
#   ./cajeta_tests.sh shard=16                     # 16 parallel workers (default 32)
#   ./cajeta_tests.sh stop                         # kill every running test/shard
#
# Anything beginning with `--` is passed straight to the test binary, so any
# gtest flag works. `shard=N` sets the number of parallel workers. The lone arg
# `stop` kills all running cajeta tests.
#
# Execution model (parallel path): N workers each pull tests from their own
# bucket and run EACH test in its OWN process with a per-test timeout. A
# genuinely hung test costs at most TEST_TIMEOUT seconds (then it is killed and
# recorded as a timeout) — it does not stall its whole worker or the run. There
# is no per-shard wall-clock cap, and timed-out tests are NOT re-run.
#
# Knobs:
#   NO_BUILD=1   skip the incremental build step
#   PARALLEL=0   force serial run even without filters
#   PARALLEL=1   force parallel run even with filters
#   PARALLEL=N   use N workers (same as shard=N; shard=N takes precedence)
#   VERBOSE=1    in parallel mode, dump each shard's full gtest output
#   TEST_TIMEOUT=N  per-test wall-clock timeout in seconds (default 120). A test
#                that runs longer is killed and reported as a timeout; the rest
#                of its worker's tests continue.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# `stop` subcommand: kill every running cajeta test process — the test binary
# (direct runs and `timeout`-wrapped parallel shards) plus any other
# cajeta_tests.sh runner instances — so a hung or runaway run can be cleared
# with `./cajeta_tests.sh stop`. Handled before the build step so it never
# triggers a compile. Matching is deliberately precise: the binary is matched by
# its *process name* (`pgrep -x cajeta_test`) — so a command that merely mentions
# the path (a grep, an editor, this script's own argv) is never hit — and a
# runner only counts if it is a *shell executing this script* — never this `stop`
# invocation, another `stop`, an editor with the file open, or this `pgrep`.
if [ "${1:-}" = "stop" ]; then
    self=$$
    victims=()
    # 1. The test binary itself (direct + timeout-wrapped shards), matched by
    #    process name so only real running binaries are hit.
    while IFS= read -r pid; do
        [ -n "$pid" ] && [ "$pid" != "$self" ] && victims+=("$pid")
    done < <(pgrep -x cajeta_test 2>/dev/null || true)
    # 2. Other runner-script instances: a shell whose argv runs cajeta_tests.sh
    #    with something other than `stop`. Inspect each candidate's argv so we
    #    don't kill an editor or a second `stop`.
    while IFS= read -r pid; do
        [ -z "$pid" ] || [ "$pid" = "$self" ] && continue
        args=$(ps -o args= -p "$pid" 2>/dev/null) || continue
        # Skip `-c` invocations (a command string that merely mentions the path,
        # e.g. another tool or `bash -c`), and any `stop` invocation. A genuine
        # runner is a shell with the script as its program argument.
        case "$args" in *" -c "*) continue ;; esac
        case "$args" in
            *sh*cajeta_tests.sh*)
                case "$args" in *" stop"|*" stop "*) continue ;; esac
                victims+=("$pid") ;;
        esac
    done < <(pgrep -f 'cajeta_tests\.sh' 2>/dev/null || true)
    # Dedup.
    if [ ${#victims[@]} -gt 0 ]; then
        IFS=$'\n' victims=($(printf '%s\n' "${victims[@]}" | sort -un)); unset IFS
    fi
    if [ ${#victims[@]} -eq 0 ]; then
        echo ">> No running cajeta tests found."
        exit 0
    fi
    echo ">> Stopping ${#victims[@]} cajeta test process(es): ${victims[*]}"
    kill "${victims[@]}" 2>/dev/null || true
    # Give them a moment to unwind, then SIGKILL any straggler that ignored TERM.
    sleep 1
    for p in "${victims[@]}"; do
        if kill -0 "$p" 2>/dev/null; then kill -9 "$p" 2>/dev/null || true; fi
    done
    echo ">> Done."
    exit 0
fi

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
shard_arg=""
for arg in "$@"; do
    case "$arg" in
        shard=*) shard_arg="${arg#shard=}" ;;
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
# An explicit shard=N always means "run the parallel path with N workers".
if [[ "$shard_arg" =~ ^[0-9]+$ ]] && [ "$shard_arg" -ge 1 ]; then
    should_parallel=1
fi

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

# Determine the worker count. Each worker runs tests one at a time, each in its
# own short-lived process (see the launch loop), so the worker count is the
# number of concurrent test processes. Default is 32 (the maximum); override
# with `shard=N` (preferred) or PARALLEL=N. An explicit override is honored
# verbatim — including above 32 — since the caller is asking for it.
if [[ "$shard_arg" =~ ^[0-9]+$ ]] && [ "$shard_arg" -ge 1 ]; then
    shards="$shard_arg"
elif [[ "${PARALLEL:-}" =~ ^[0-9]+$ ]] && [ "${PARALLEL}" -gt 1 ]; then
    shards="${PARALLEL}"
else
    shards=32
fi
[ "$shards" -lt 1 ] && shards=1

# Per-test wall-clock timeout. A single test that exceeds this is killed and
# recorded as a timeout; its worker moves on to the next test. There is no
# per-shard cap — a worker runs until its whole bucket is exhausted.
TEST_TIMEOUT="${TEST_TIMEOUT:-120}"

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

# Round-robin distribute test names into per-shard buckets. Each bucket is a
# newline-separated list of test names (one per line) that its worker iterates,
# running every test in its own process. shard_total tracks how many tests each
# shard owns, so the live display can show per-shard completion.
declare -a shard_list
declare -a shard_total
for ((s=0; s<shards; s++)); do shard_list[$s]=""; shard_total[$s]=0; done
for ((i=0; i<num_tests; i++)); do
    s=$((i % shards))
    shard_list[$s]+="${tests[$i]}"$'\n'
    shard_total[$s]=$(( shard_total[$s] + 1 ))
done

tmpdir=$(mktemp -d -t cajeta_test_shards.XXXXXX)
trap 'rm -rf "$tmpdir"' EXIT

# Live progress display: redraw an overall completion bar + one row per shard
# in place using ANSI cursor control. Gated on an interactive stdout; in CI /
# pipes / VERBOSE mode (or with TUI=0) it falls back to a silent wait + the
# end-of-run summary, byte-identical to the non-interactive behavior. The live
# view needs the per-test `[ RUN ]`/`[ OK ]` lines that --gtest_brief=1
# suppresses, so shards emit full output when live — the `[ PASSED ]/[ FAILED ]`
# lines the aggregator parses are printed either way.
live=0
if [ -t 1 ] && [ "${TUI:-1}" != "0" ] && [ "${VERBOSE:-}" != "1" ]; then
    live=1
fi
if [ "$live" = "1" ]; then shard_brief="--gtest_brief=0"; else shard_brief="--gtest_brief=1"; fi

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
        # Disable set -e so a crashing/failing/timed-out test still falls
        # through to the next test (set -e would terminate the worker
        # mid-bucket). Each test runs in ITS OWN process under
        # `timeout --kill-after=10 $TEST_TIMEOUT`: SIGTERM at the deadline
        # (exit 124), SIGKILL 10s later if it ignores the term (exit 137).
        # A hung test therefore costs at most TEST_TIMEOUT and the worker
        # keeps going — no per-shard wall-clock cap, no whole-shard stall.
        # On a non-zero exit with no gtest [ FAILED ] line for the test
        # (crash / timeout), append a synthetic marker the aggregator counts.
        set +e
        tf="$tmpdir/t_${s}.out"
        while IFS= read -r t; do
            [ -z "$t" ] && continue
            timeout --kill-after=10 "$TEST_TIMEOUT" \
                env CAJETA_SOURCE_ROOT="$SCRIPT_DIR" "$TEST_BIN" \
                "--gtest_filter=$t" \
                "$shard_brief" \
                > "$tf" 2>&1
            trc=$?
            cat "$tf" >> "$out_file"
            if [ "$trc" -ne 0 ]; then
                case "$trc" in
                    124|137) printf '>>> TIMEOUT %s (killed after %ss)\n' \
                                "$t" "$TEST_TIMEOUT" >> "$out_file" ;;
                    *)
                        # Only a true crash if gtest produced NO [ FAILED ]
                        # report for this test. A normal assertion failure
                        # also exits non-zero (1) but prints [ FAILED ] — that
                        # is already counted as a failure, not a crash.
                        if ! grep -qE '^\[  FAILED  \]' "$tf"; then
                            printf '>>> CRASH %s (exit %s)\n' \
                                "$t" "$trc" >> "$out_file"
                        fi
                        ;;
                esac
            fi
        done <<< "${shard_list[$s]}"
        echo 0 > "$exit_file"
    } 2>/dev/null &
    pids+=($!)
done

# --- Live progress dashboard (interactive only) ----------------------------
# A full-screen dashboard on the terminal's ALTERNATE screen: an overall bar
# plus one row per shard showing its mini-bar, done/assigned, the test it is
# currently running, and how long that test has been going (flagged `!` past
# 30s so a hang is obvious). Each shard's "done" count = its emitted `[ OK ]` +
# inline `[ FAILED ] ... (N ms)` lines; its current test = the last `[ RUN ]`.
# Absolute cursor positioning (home + clear-to-EOL) repaints in place, so all
# shards render regardless of count. Loops until every shard drops its .exit
# sentinel; the `wait` below then reaps the already-finished jobs instantly. The
# alternate screen leaves no scrollback — the end-of-run summary prints normally.
if [ "$live" = "1" ]; then
    draw_bar() {  # draw_bar <done> <total> <width>
        local d=$1 t=$2 w=$3 filled i
        if [ "$t" -le 0 ]; then filled=0; else filled=$(( d * w / t )); fi
        [ "$filled" -gt "$w" ] && filled=$w
        printf '['
        for ((i=0; i<filled; i++)); do printf '#'; done
        for ((i=filled; i<w; i++)); do printf '-'; done
        printf ']'
    }
    # Per-shard current-test tracking, so the dashboard can show how long the
    # running test has been going (and flag a stuck one).
    declare -a cur_name cur_since
    for ((s=0; s<shards; s++)); do cur_name[$s]=""; cur_since[$s]=$start_time; done
    term_lines=$(tput lines 2>/dev/null || echo 40)
    avail=$(( term_lines - 3 ))   # rows for shards = height minus header + blank + margin
    [ "$avail" -lt 1 ] && avail=1
    # Enter the alternate screen, hide the cursor, clear. Augment the EXIT trap
    # (and add INT/TERM) so a normal finish OR a Ctrl-C both restore the
    # screen/cursor and still clean the shard tmpdir.
    printf '\033[?1049h\033[?25l\033[2J'
    trap 'printf "\033[?25h\033[?1049l" 2>/dev/null; rm -rf "$tmpdir" 2>/dev/null' EXIT
    trap 'exit 130' INT TERM
    while :; do
        all_done=1
        total_done=0
        block=""
        shown=0
        for ((s=0; s<shards; s++)); do
            o="$tmpdir/shard_${s}.out"
            d=$(grep -cE '^\[ *OK *\]|^\[  FAILED  \] [A-Za-z_].*\([0-9]+ ms\)$|^>>> (TIMEOUT|CRASH) ' "$o" 2>/dev/null || true)
            d=${d:-0}
            total_done=$((total_done + d))
            asg=${shard_total[$s]:-0}
            if [ -f "$tmpdir/shard_${s}.exit" ]; then
                cur="done"; tel=0; flag=" "
            else
                all_done=0
                cur=$(grep -E '^\[ *RUN *\]' "$o" 2>/dev/null | tail -1 | sed -E 's/^\[ *RUN *\] //')
                cur=${cur:-<starting>}
                if [ "$cur" != "${cur_name[$s]}" ]; then cur_name[$s]="$cur"; cur_since[$s]=$SECONDS; fi
                tel=$(( SECONDS - cur_since[$s] ))
                if [ "$tel" -ge 30 ]; then flag="!"; else flag=" "; fi
            fi
            if [ "$shown" -lt "$avail" ]; then
                # No per-shard progress bar: gtest gives no progress inside a
                # single test, and a shard runs one test at a time, so a
                # completion bar barely moves. Show the running test and how
                # long it has been going (the live, second-by-second feedback),
                # with the shard's done/assigned count alongside.
                if [ "$cur" = "done" ]; then
                    block+=$(printf 'shard %2d  %4d/%-4d  done' "$s" "$d" "$asg")
                else
                    block+=$(printf 'shard %2d  %4d/%-4d  %s%-44s %4ds' \
                        "$s" "$d" "$asg" "$flag" "${cur:0:44}" "$tel")
                fi
                block+=$'\n'
                shown=$((shown + 1))
            fi
        done
        if [ "$shards" -gt "$avail" ]; then
            block+=$(printf '... %d more shard(s) not shown (terminal too short)' "$((shards - avail))")
            block+=$'\n'
        fi
        el=$((SECONDS - start_time))
        header=$(printf 'Cajeta tests  %s  %5d/%-5d   %d shards   %ds' \
            "$(draw_bar "$total_done" "$num_tests" 34)" "$total_done" "$num_tests" "$shards" "$el")
        # Home the cursor, paint header + a blank spacer + the shard rows (each
        # cleared to end-of-line), then clear the rest of the screen below.
        printf '\033[H%s\033[K\n\033[K\n' "$header"
        printf '%s' "${block%$'\n'}" | while IFS= read -r ln; do printf '%s\033[K\n' "$ln"; done
        printf '\033[J'
        [ "$all_done" = "1" ] && break
        sleep 0.4
    done
    # Leave the alternate screen so the summary prints normally; restore the
    # plain tmpdir-only EXIT trap and drop the INT/TERM handlers.
    printf '\033[?25h\033[?1049l'
    trap 'rm -rf "$tmpdir"' EXIT
    trap - INT TERM
fi

# Wait for every shard. Use plain `wait` so a single shard's nonzero exit
# doesn't trip `set -e` before the others finish.
for pid in "${pids[@]}"; do
    wait "$pid" || true
done

elapsed=$((SECONDS - start_time))

# Aggregate across every shard's output. Each test ran in its own process, so
# the per-process gtest summary lines are what we count:
#   passed   = sum of N over `[  PASSED  ] N test(s).` lines (one per process;
#              N is 1 for a passing test, 0 for a failing one — so summing is
#              correct regardless of --gtest_brief).
#   failed   = per-test `[  FAILED  ] Suite.test (N ms)` lines (the identifier
#              form, not the `[  FAILED  ] N test,` count-summary which begins
#              with a digit).
#   timeouts = synthetic `>>> TIMEOUT Suite.test ...` markers the worker wrote
#              when `timeout` killed a test (exit 124/137).
#   crashes  = synthetic `>>> CRASH Suite.test ...` markers for any other
#              non-zero test exit with no gtest report (segfault / abort).
# Timed-out tests are NOT re-run — a timeout is reported as a failure.
total_passed=0
total_failed=0
failure_lines=()
timeout_lines=()
crash_lines=()

for ((s=0; s<shards; s++)); do
    out_file="$tmpdir/shard_${s}.out"

    passed=$(grep -oE '^\[  PASSED  \] [0-9]+ test' "$out_file" 2>/dev/null \
                | grep -oE '[0-9]+' | awk '{n+=$1} END{print n+0}')
    passed=${passed:-0}
    failed=$(grep -cE '^\[  FAILED  \] [A-Za-z_].*\([0-9]+ ms\)$' "$out_file" 2>/dev/null || true)
    failed=${failed:-0}

    total_passed=$((total_passed + passed))
    total_failed=$((total_failed + failed))

    if [ "$failed" -gt 0 ]; then
        while IFS= read -r line; do
            failure_lines+=("$line")
        done < <(grep -E '^\[  FAILED  \] [A-Za-z_].*\([0-9]+ ms\)$' "$out_file")
    fi
    while IFS= read -r line; do
        [ -n "$line" ] && timeout_lines+=("$line")
    done < <(grep -E '^>>> TIMEOUT ' "$out_file" 2>/dev/null || true)
    while IFS= read -r line; do
        [ -n "$line" ] && crash_lines+=("$line")
    done < <(grep -E '^>>> CRASH ' "$out_file" 2>/dev/null || true)
done

num_timeouts=${#timeout_lines[@]}
num_crashes=${#crash_lines[@]}
# Anything discovered that neither passed, failed, timed out, nor crashed was
# not actually run: a DISABLED_-prefixed test (gtest lists but skips it) or a
# runtime GTEST_SKIP (e.g. a device test with no usable GPU). Derive it so the
# tally always reconciles to Discovered.
num_skipped=$(( num_tests - total_passed - total_failed - num_timeouts - num_crashes ))
[ "$num_skipped" -lt 0 ] && num_skipped=0

echo
echo "=== Test summary ==="
echo "Discovered: $num_tests   Workers: $shards   Elapsed: ${elapsed}s"
echo "Passed: $total_passed   Failed: $total_failed   Timed out: $num_timeouts   Crashed: $num_crashes   Skipped/disabled: $num_skipped"

if [ ${#failure_lines[@]} -gt 0 ]; then
    echo
    echo "Failed tests:"
    for line in "${failure_lines[@]}"; do
        echo "  $line"
    done
fi

if [ "$num_timeouts" -gt 0 ]; then
    echo
    echo "Timed out (killed after ${TEST_TIMEOUT}s, not re-run):"
    for line in "${timeout_lines[@]}"; do
        echo "  ${line#>>> TIMEOUT }"
    done
fi

if [ "$num_crashes" -gt 0 ]; then
    echo
    echo "Crashed:"
    for line in "${crash_lines[@]}"; do
        echo "  ${line#>>> CRASH }"
    done
    echo
    echo "Re-run a crashing test in isolation with:"
    echo "  PARALLEL=0 ./cajeta_tests.sh <SuiteName.testName>"
fi

if [ "${VERBOSE:-}" = "1" ]; then
    echo
    echo "=== Per-shard output ==="
    for ((s=0; s<shards; s++)); do
        echo "----- shard ${s} -----"
        cat "$tmpdir/shard_${s}.out"
    done
fi

if [ "$total_failed" -gt 0 ] || [ "$num_timeouts" -gt 0 ] || [ "$num_crashes" -gt 0 ]; then
    exit 1
fi
exit 0
