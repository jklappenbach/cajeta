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
#   KEEP_LOGS=dir  (parallel mode only) persist each shard's raw output —
#                the full per-test gtest text including assertion detail and
#                the synthetic >>> CRASH / >>> TIMEOUT markers — into `dir`
#                before the shard tmpdir is deleted. Relative paths resolve
#                against the repo root (this script cd's there); CI uses
#                KEEP_LOGS=run-logs (gitignored) and uploads it as an
#                artifact so a failing platform leg can be diagnosed from
#                the run page without reproducing on that platform.

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

TEST_BIN="${TEST_BIN:-build/test/cajeta_test}"

# CAJETA_TESTS_FILE injects a pre-discovered test list (one Suite.test per line),
# used by test/harness/suite_split_test.sh to exercise the scheduler without a
# built binary. When set, skip the build guards + the binary-existence check —
# nothing is compiled or executed (the harness tests only run PLAN_ONLY).
if [ -z "${CAJETA_TESTS_FILE:-}" ]; then
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

# Stdlib reuse (parallel sweep only): prime the stdlib ONCE per suite-process and
# share it across that process's tests, instead of re-parsing+re-codegen'ing the
# ~14s stdlib for every test. This is the dominant speed lever for a full sweep
# (many-hours -> ~1.3h) and — because each process finishes far sooner — it also
# LOWERS sustained memory pressure vs the non-reuse path. Correct as of the
# per-thread reuse gate (concurrent-compile) + wildcard-array-element fix; the
# harness still auto-falls-back per-test on any reuse hazard. On by default here;
# opt out with REUSE=0. Serial single-suite/-test runs (the debug path, exec'd
# earlier) stay fully isolated — reuse never changes what a dev is debugging.
# Exported so every shard child process inherits it.
if [ "${REUSE:-1}" != "0" ]; then export CAJETA_STDLIB_REUSE=1; else unset CAJETA_STDLIB_REUSE; fi

# Determine the worker count = the number of concurrent test processes. An
# explicit `shard=N` / PARALLEL=N is honored verbatim (the caller asked for it).
# Otherwise the default is MEMORY-AWARE: use every core, but cap so the
# concurrent suite-processes can't exhaust RAM and swap-thrash / OOM. Each
# suite-process peaks around 0.4-0.9 GB under reuse (measured); budget
# PER_PROC_MB conservatively and size against CURRENTLY-available RAM, so the run
# automatically backs off when the box is already loaded (e.g. a second sweep in
# a sibling repo) and uses full width when it's idle.
if [[ "$shard_arg" =~ ^[0-9]+$ ]] && [ "$shard_arg" -ge 1 ]; then
    shards="$shard_arg"
elif [[ "${PARALLEL:-}" =~ ^[0-9]+$ ]] && [ "${PARALLEL}" -gt 1 ]; then
    shards="${PARALLEL}"
else
    cores=$(nproc 2>/dev/null || echo 8)
    avail_mb=$(awk '/^MemAvailable:/{print int($2/1024)}' /proc/meminfo 2>/dev/null)
    [ -z "$avail_mb" ] && avail_mb=$(( cores * 2048 ))   # /proc absent (macOS): assume ~2GB/core
    per_proc_mb="${PER_PROC_MB:-1300}"
    mem_cap=$(( avail_mb * 3 / 4 / per_proc_mb ))        # spend ~75% of available RAM
    [ "$mem_cap" -lt 1 ] && mem_cap=1
    shards=$cores
    [ "$shards" -gt "$mem_cap" ] && shards=$mem_cap
    echo ">> Auto shards=$shards (cores=$cores, availRAM=${avail_mb}MB, ~${per_proc_mb}MB/proc; override with shard=N or PER_PROC_MB=)" >&2
fi
[ "$shards" -lt 1 ] && shards=1

# Per-test wall-clock timeout. A single test that exceeds this is killed and
# recorded as a timeout; its worker moves on to the next test. There is no
# per-shard cap — a worker runs until its whole bucket is exhausted.
TEST_TIMEOUT="${TEST_TIMEOUT:-120}"

# Portable per-test timeout wrapper. GNU coreutils `timeout` is standard on
# Linux and MSYS2 but ABSENT on macOS (where coreutils, if brew-installed,
# ships it as `gtimeout`). Without this detection every test invocation on
# macOS failed with exit 127 ("timeout: command not found") and was recorded
# as a crash — silently, because the macOS release-test step is non-fatal.
# Prefer `timeout`, fall back to `gtimeout`, and as a last resort run the test
# directly (no timeout — a hung test could stall its worker, but coverage is
# far more valuable than losing it entirely to a missing wrapper).
if command -v timeout >/dev/null 2>&1; then
    TIMEOUT_CMD="timeout"
elif command -v gtimeout >/dev/null 2>&1; then
    TIMEOUT_CMD="gtimeout"
else
    TIMEOUT_CMD=""
    echo ">> WARNING: no 'timeout'/'gtimeout' found — running tests without a per-test timeout." >&2
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
tests=()
if [ -n "${CAJETA_TESTS_FILE:-}" ]; then
    # Injected list (test seam): one fully-qualified Suite.test per line, in the
    # order the suites should group. No binary invoked.
    while IFS= read -r line || [ -n "$line" ]; do
        [ -z "$line" ] && continue
        tests+=("$line")
    done < "$CAJETA_TESTS_FILE"
else
    raw_list=$(CAJETA_SOURCE_ROOT="$SCRIPT_DIR" "$TEST_BIN" --gtest_list_tests "${list_filter_args[@]}" 2>/dev/null || true)
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
fi

num_tests=${#tests[@]}
if [ "$num_tests" -eq 0 ]; then
    echo "error: no tests discovered via --gtest_list_tests" >&2
    exit 1
fi

# BATCH mode (default on): shard by SUITE and run each suite as ONE process so
# the in-process stdlib cache (test/jit/JitTestHelper.cpp StdlibCache) primes
# the stdlib once and reuses it across the suite's tests. That prime — re-parse
# + IR + codegen of the whole cajeta stdlib — is the dominant per-process cost
# (~5s Linux / ~15s macOS / ~40-60s Windows), paid once per process. One
# process per test (BATCH=0) pays it 474×; one per suite pays it ~(#suites)×.
# A suite whose batched process crashes or hangs is automatically re-run
# test-by-test (see run_suite_batch) so crash isolation and per-test attribution
# are preserved. BATCH=0 restores strict one-process-per-test.
BATCH="${BATCH:-1}"
# Upper bound on a single suite's batched wall-clock before it's treated as hung
# and dropped to the per-test fallback. The deadline scales with suite size
# (n * TEST_TIMEOUT) but never exceeds this, so one stuck test can't strand a
# worker for the full n*TEST_TIMEOUT.
BATCH_CAP="${BATCH_CAP:-1800}"
# Max tests per batched chunk. A suite with more than this many selected tests is
# split into ceil(n/BATCH_MAX) contiguous chunks that bin-pack across shards and
# run concurrently — so one oversized suite is no longer a single-shard long pole
# (e.g. the 49-test ParallelStreamP1Tests, which alone dominated the ~95min
# Windows release-test leg). Each chunk still runs as ONE process, so the stdlib
# prime is reused within the chunk; splitting pays the prime ceil(n/BATCH_MAX)
# times instead of once, a net win because the extra shards were otherwise idle.
# A suite with <= BATCH_MAX tests is exactly one chunk (unchanged behavior).
BATCH_MAX="${BATCH_MAX:-16}"

# Group discovered tests by suite using INDEXED arrays only — macOS ships bash
# 3.2, which has no `declare -A` associative arrays. gtest --gtest_list_tests
# emits a suite's tests contiguously and discovery preserves that order, so a
# single pass that opens a new group whenever the suite prefix changes captures
# exact membership (a partially-selected suite batches only its selected tests,
# never the whole `Suite.*`). suite_tests_idx[i] / suite_count_idx[i] parallel
# suites[i].
declare -a suites
declare -a suite_tests_idx   # i -> newline-separated "Suite.test" list
declare -a suite_count_idx
_cur_suite=""
_ci=-1
for t in "${tests[@]}"; do
    sname="${t%%.*}"
    if [ "$sname" != "$_cur_suite" ]; then
        _cur_suite="$sname"
        _ci=$(( _ci + 1 ))
        suites[$_ci]="$sname"
        suite_tests_idx[$_ci]=""
        suite_count_idx[$_ci]=0
    fi
    suite_tests_idx[$_ci]+="${t}"$'\n'
    suite_count_idx[$_ci]=$(( suite_count_idx[$_ci] + 1 ))
done
num_suites=${#suites[@]}

# Build the CHUNK list — the scheduling unit. In BATCH mode a suite with more
# than BATCH_MAX selected tests is split into ceil(n/BATCH_MAX) contiguous slices
# (each its own batched process, priming once); a suite <= BATCH_MAX is one chunk.
# In BATCH=0 each test is its own chunk (count 1). Parallel indexed arrays only
# (bash 3.2: no associative arrays). chunk_list[i] is the newline-joined test list.
declare -a chunk_name chunk_list chunk_count
num_chunks=0
if [ "$BATCH" = "1" ]; then
    for ((i=0; i<num_suites; i++)); do
        _sn="${suites[$i]}"; _cnt="${suite_count_idx[$i]}"
        if [ "$_cnt" -le "$BATCH_MAX" ]; then
            chunk_name[$num_chunks]="$_sn"
            chunk_list[$num_chunks]="${suite_tests_idx[$i]}"
            chunk_count[$num_chunks]="$_cnt"
            num_chunks=$(( num_chunks + 1 ))
        else
            _nparts=$(( (_cnt + BATCH_MAX - 1) / BATCH_MAX ))
            _part=1; _incount=0; _slice=""
            while IFS= read -r _t; do
                [ -z "$_t" ] && continue
                _slice+="${_t}"$'\n'
                _incount=$(( _incount + 1 ))
                if [ "$_incount" -eq "$BATCH_MAX" ]; then
                    chunk_name[$num_chunks]="${_sn}[${_part}/${_nparts}]"
                    chunk_list[$num_chunks]="$_slice"
                    chunk_count[$num_chunks]="$_incount"
                    num_chunks=$(( num_chunks + 1 ))
                    _part=$(( _part + 1 )); _incount=0; _slice=""
                fi
            done <<< "${suite_tests_idx[$i]}"
            if [ "$_incount" -gt 0 ]; then
                chunk_name[$num_chunks]="${_sn}[${_part}/${_nparts}]"
                chunk_list[$num_chunks]="$_slice"
                chunk_count[$num_chunks]="$_incount"
                num_chunks=$(( num_chunks + 1 ))
            fi
        fi
    done
else
    for ((i=0; i<num_tests; i++)); do
        chunk_name[$num_chunks]="${tests[$i]}"
        chunk_list[$num_chunks]="${tests[$i]}"$'\n'
        chunk_count[$num_chunks]=1
        num_chunks=$(( num_chunks + 1 ))
    done
fi

# Cap shards at the number of schedulable units (CHUNKS in BATCH mode, else
# tests) — extra shards just sit empty. A split suite has more chunks than the
# one suite, so the cap must count chunks, not suites, or a single large suite
# would pin the run back to one shard.
if [ "$BATCH" = "1" ]; then unit_count=$num_chunks; else unit_count=$num_tests; fi
if [ "$shards" -gt "$unit_count" ]; then shards=$unit_count; fi

# Distribute work into per-shard buckets. shard_total tracks how many TESTS each
# shard owns (the live display denominator), regardless of unit granularity.
declare -a shard_list
declare -a shard_total
for ((s=0; s<shards; s++)); do shard_list[$s]=""; shard_total[$s]=0; done
if [ "$BATCH" = "1" ]; then
    # Longest-processing-time first: assign each suite (largest test-count first)
    # to the currently least-loaded shard, so workers get balanced test totals
    # even though suites vary widely in size. Buckets carry the suite INDEX so
    # the worker looks up its exact test list without an associative array.
    sorted_suites=$(for ((i=0; i<num_suites; i++)); do
        printf '%s\t%s\n' "${suite_count_idx[$i]}" "$i"
    done | sort -rn -k1,1)
    while IFS=$'\t' read -r cnt idx; do
        [ -z "$idx" ] && continue
        min_s=0; min_v=${shard_total[0]}
        for ((s=1; s<shards; s++)); do
            if [ "${shard_total[$s]}" -lt "$min_v" ]; then min_v=${shard_total[$s]}; min_s=$s; fi
        done
        shard_list[$min_s]+="${idx}"$'\n'
        shard_total[$min_s]=$(( shard_total[$min_s] + cnt ))
    done <<< "$sorted_suites"
else
    for ((i=0; i<num_tests; i++)); do
        s=$((i % shards))
        shard_list[$s]+="${tests[$i]}"$'\n'
        shard_total[$s]=$(( shard_total[$s] + 1 ))
    done
fi

# PLAN_ONLY: emit the chunk->shard assignment and exit before spawning anything.
# Used by test/harness/suite_split_test.sh to assert the scheduling math without
# building or running the binary. Longest-processing-time-first over chunks, the
# same heuristic the executor uses. Output contract (stdout):
#   PLAN shards=<S> batch=<0|1> batch_max=<N> chunks=<C> tests=<T>
#   CHUNK <idx> shard=<s> count=<k> name=<display> tests=<t1,t2,...>
if [ -n "${PLAN_ONLY:-}" ]; then
    plan_shards=$shards
    [ "$plan_shards" -gt "$num_chunks" ] && plan_shards=$num_chunks
    [ "$plan_shards" -lt 1 ] && plan_shards=1
    declare -a _sh_load
    for ((s=0; s<plan_shards; s++)); do _sh_load[$s]=0; done
    _order=$(for ((i=0; i<num_chunks; i++)); do
        printf '%s\t%s\n' "${chunk_count[$i]}" "$i"
    done | sort -rn -k1,1 -s)
    declare -a _chunk_shard
    while IFS=$'\t' read -r _cc _ci2; do
        [ -z "$_ci2" ] && continue
        _ms=0; _mv=${_sh_load[0]}
        for ((s=1; s<plan_shards; s++)); do
            if [ "${_sh_load[$s]}" -lt "$_mv" ]; then _mv=${_sh_load[$s]}; _ms=$s; fi
        done
        _chunk_shard[$_ci2]=$_ms
        _sh_load[$_ms]=$(( _sh_load[$_ms] + _cc ))
    done <<< "$_order"
    printf 'PLAN shards=%s batch=%s batch_max=%s chunks=%s tests=%s\n' \
        "$plan_shards" "$BATCH" "$BATCH_MAX" "$num_chunks" "$num_tests"
    for ((i=0; i<num_chunks; i++)); do
        _csv=$(printf '%s' "${chunk_list[$i]}" | grep -v '^$' | paste -sd, -)
        printf 'CHUNK %s shard=%s count=%s name=%s tests=%s\n' \
            "$i" "${_chunk_shard[$i]}" "${chunk_count[$i]}" "${chunk_name[$i]}" "$_csv"
    done
    exit 0
fi

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

# Run ONE test in its own process, appending its output + a synthetic
# >>> CRASH / >>> TIMEOUT marker (counted by the aggregator) to $1. This is the
# strict-isolation path: BATCH=0 uses it directly, and the batch fallback uses
# it to recover attribution for a suite whose batched process died.
#
# Each test writes a one-line `>> Suite.test ... ` breadcrumb (the name FIRST,
# with NO trailing newline) BEFORE it runs, then completes the line with its
# result + newline AFTER. So a clean log reads one greppable line per test
# (`grep '^>> '` → the whole pass/fail list), and if the process is killed
# mid-test the dangling result-less `>> Suite.test ... ` line pinpoints exactly
# which test was executing when it died. The result words are distinct from the
# gtest [ PASSED ]/[ FAILED ] lines and the >>> markers, so counting is
# unaffected; full gtest output still follows for forensics.
run_one_test() {
    local out_file="$1" t="$2" tf trc
    tf="${out_file}.t"
    printf '>> %s ... ' "$t" >> "$out_file"
    if [ -n "$TIMEOUT_CMD" ]; then
        "$TIMEOUT_CMD" --kill-after=10 "$TEST_TIMEOUT" \
            env CAJETA_SOURCE_ROOT="$SCRIPT_DIR" "$TEST_BIN" \
            "--gtest_filter=$t" "$shard_brief" > "$tf" 2>&1
    else
        env CAJETA_SOURCE_ROOT="$SCRIPT_DIR" "$TEST_BIN" \
            "--gtest_filter=$t" "$shard_brief" > "$tf" 2>&1
    fi
    trc=$?
    case "$trc" in
        0)       printf 'PASS\n'              >> "$out_file" ;;
        124|137) printf 'TIMEOUT (%ss)\n' "$TEST_TIMEOUT" >> "$out_file" ;;
        *) if grep -qE '^\[  FAILED  \]' "$tf"; then
               printf 'FAIL\n'               >> "$out_file"
           else
               printf 'CRASH (exit %s)\n' "$trc" >> "$out_file"
           fi ;;
    esac
    cat "$tf" >> "$out_file"
    if [ "$trc" -ne 0 ]; then
        case "$trc" in
            124|137) printf '>>> TIMEOUT %s (killed after %ss)\n' "$t" "$TEST_TIMEOUT" >> "$out_file" ;;
            *) if ! grep -qE '^\[  FAILED  \]' "$tf"; then
                   printf '>>> CRASH %s (exit %s)\n' "$t" "$trc" >> "$out_file"
               fi ;;
        esac
    fi
    rm -f "$tf"
}

# Run a whole suite as ONE process so the stdlib cache primes once and is reused
# across its tests. The filter is the exact list of the suite's selected tests
# (not `Suite.*`), so a partially-selected suite runs only what was selected.
# Deadline scales with suite size, capped at BATCH_CAP.
#
# Trust the batched output IFF gtest ran to completion — its terminal
# "[==========] N ... ran." line is present (true for a clean pass AND for a run
# with ordinary [ FAILED ] assertions, which the aggregator already counts). If
# that line is absent (segfault/abort mid-suite) or the process hit its deadline
# (exit 124/137), discard the partial output and re-run every test in the suite
# individually via run_one_test — restoring exact crash/timeout attribution.
run_suite_batch() {
    local out_file="$1" idx="$2" sname tf brc deadline n filter list
    sname="${suites[$idx]}"
    n="${suite_count_idx[$idx]}"
    list="${suite_tests_idx[$idx]}"
    deadline=$(( n * TEST_TIMEOUT ))
    [ "$deadline" -gt "$BATCH_CAP" ] && deadline=$BATCH_CAP
    [ "$deadline" -lt "$TEST_TIMEOUT" ] && deadline=$TEST_TIMEOUT
    filter="${list//$'\n'/:}"; filter="${filter%:}"
    tf="${out_file}.b"
    if [ -n "$TIMEOUT_CMD" ]; then
        "$TIMEOUT_CMD" --kill-after=10 "$deadline" \
            env CAJETA_SOURCE_ROOT="$SCRIPT_DIR" "$TEST_BIN" \
            "--gtest_filter=$filter" "$shard_brief" > "$tf" 2>&1
    else
        env CAJETA_SOURCE_ROOT="$SCRIPT_DIR" "$TEST_BIN" \
            "--gtest_filter=$filter" "$shard_brief" > "$tf" 2>&1
    fi
    brc=$?
    if [ "$brc" -eq 0 ] || grep -qE '^\[==========\] .* ran\.' "$tf"; then
        cat "$tf" >> "$out_file"
    else
        printf '>>> BATCH-FALLBACK %s (suite process exit %s; re-running %s test(s) individually)\n' \
            "$sname" "$brc" "$n" >> "$out_file"
        local t
        while IFS= read -r t; do
            [ -z "$t" ] && continue
            run_one_test "$out_file" "$t"
        done <<< "$list"
    fi
    rm -f "$tf"
}

if [ "$BATCH" = "1" ]; then
    echo ">> Running $num_tests tests across $shards shards (BATCH: $num_suites suites, one process each + per-test crash fallback)..."
else
    echo ">> Running $num_tests tests across $shards shards (one process per test)..."
fi
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
        # Disable set -e so a crashing/failing/timed-out unit still falls
        # through to the next one (set -e would terminate the worker mid-bucket).
        # In BATCH mode each unit is a SUITE run as one process (stdlib primed
        # once, reused across its tests) with an automatic per-test fallback on
        # crash/hang; in BATCH=0 each unit is a single test in its own process.
        # Either way a crash/timeout is bounded and recorded with a synthetic
        # marker the aggregator counts — no whole-shard stall.
        set +e
        while IFS= read -r unit; do
            [ -z "$unit" ] && continue
            if [ "$BATCH" = "1" ]; then
                run_suite_batch "$out_file" "$unit"
            else
                run_one_test "$out_file" "$unit"
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

# KEEP_LOGS: persist the raw shard outputs (full per-test gtest text +
# crash/timeout markers) before the EXIT trap deletes the shard tmpdir.
# Best-effort — a log-copy failure must never change the run's verdict.
if [ -n "${KEEP_LOGS:-}" ]; then
    if mkdir -p "$KEEP_LOGS" 2>/dev/null \
        && cp "$tmpdir"/shard_*.out "$KEEP_LOGS"/ 2>/dev/null; then
        echo
        echo ">> Shard logs kept in: $KEEP_LOGS"
    else
        echo ">> WARNING: KEEP_LOGS=$KEEP_LOGS — could not persist shard logs" >&2
    fi
fi

if [ "$total_failed" -gt 0 ] || [ "$num_timeouts" -gt 0 ] || [ "$num_crashes" -gt 0 ]; then
    exit 1
fi
exit 0
