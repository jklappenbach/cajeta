#!/bin/bash
# Tests for cajeta_tests.sh's fork server mode (compile-cache 6.1.4): each
# shard worker starts one test binary that primes once and forks a child per
# chunk, instead of a process per chunk.
#
# Runs the REAL parallel sweep on a few suites twice, under the server mode
# and under FORK=0, and compares what the shards report:
#   verdicts  : the same per-test OK/FAIL lines both ways;
#   primes    : the server run primes once per shard, the process run once
#               per chunk;
#   fallback  : a chunk holding the crash probe is re-run test by test and
#               the probe's crash is attributed to the probe alone.
# Needs a built test binary (NO_BUILD=1 is passed); skips itself without one.

set -u
HARNESS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HARNESS_DIR/../.." && pwd)"
CT="$ROOT/cajeta_tests.sh"
TEST_BIN="${TEST_BIN:-$ROOT/build/test/cajeta_test}"
TMP="$ROOT/tmp/fork_server_test.$$"
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

pass=0; fail=0
ok()   { pass=$((pass+1)); printf '  ok   %s\n' "$1"; }
bad()  { fail=$((fail+1)); printf '  FAIL %s\n' "$1"; [ -n "${2:-}" ] && printf '        %s\n' "$2"; }
check(){ if [ "$2" = "$3" ]; then ok "$1"; else bad "$1" "expected [$3] got [$2]"; fi; }

if [ ! -x "$TEST_BIN" ]; then
    echo "fork_server_test: no built binary at $TEST_BIN, skipping"
    echo "PASS=0 FAIL=0 (skipped)"
    exit 0
fi

SUITES="SharedStdlibDefaultTests ClosureTransferCaptureTests ZoneOffsetTests ForkCrashProbe"

# run_sweep <logdir> [env assignments...]
run_sweep() {
    local logs="$1"; shift
    # shellcheck disable=SC2086
    env "$@" NO_BUILD=1 PARALLEL=2 TUI=0 PROGRESS=0 KEEP_LOGS="$logs" \
        CAJETA_PRIME_TIMING=1 CAJETA_FORK_PROBE_CRASH=1 "$CT" $SUITES 2>&1
}

verdicts() {   # verdicts <logdir>: sorted "STATUS Suite.test" lines across the shards
    cat "$1"/shard_*.out 2>/dev/null \
        | sed -nE 's/^\[       OK \] ([^ ]+) \([0-9]+ ms\)$/OK \1/p; s/^\[  FAILED  \] ([^ ]+) \([0-9]+ ms\)$/FAIL \1/p; s/^>>> CRASH ([^ ]+) .*/CRASH \1/p' \
        | sort
}

SERVER="$(run_sweep "$TMP/server")"
PROCESS="$(run_sweep "$TMP/process" FORK=0)"

check "6.1.4 the server run reports the same pass count" \
      "$(printf '%s\n' "$SERVER" | sed -nE 's/^Passed: ([0-9]+) .*/\1/p')" \
      "$(printf '%s\n' "$PROCESS" | sed -nE 's/^Passed: ([0-9]+) .*/\1/p')"
check "6.1.4 the per-test verdicts match" "$(verdicts "$TMP/server")" "$(verdicts "$TMP/process")"
nverd=$(verdicts "$TMP/server" | grep -c .)
if [ "$nverd" -ge 10 ]; then ok "6.1.4 verdicts cover the suites ($nverd)"; else bad "6.1.4 verdicts cover the suites" "got $nverd"; fi

server_primes=$(cat "$TMP"/server/shard_*.out 2>/dev/null | grep -c '\[CAJETA_PRIME_TIMING\]')
process_primes=$(cat "$TMP"/process/shard_*.out 2>/dev/null | grep -c '\[CAJETA_PRIME_TIMING\]')
check "6.1.4 the process run primes once per JIT chunk" "$process_primes" "3"
check "6.1.4 the server run primes nowhere in the shard logs (the servers prime)" "$server_primes" "0"
nservers=$(printf '%s\n' "$SERVER" | grep -c '^>> Fork server: shard [0-9]* primed')
check "6.1.4 one server primed per shard" "$nservers" "2"

check "6.1.4 the crash probe falls back per test under the server" \
      "$(cat "$TMP"/server/shard_*.out | grep -c '^>>> BATCH-FALLBACK ForkCrashProbe')" "1"
check "6.1.4 the crash is attributed to the probe" \
      "$(cat "$TMP"/server/shard_*.out | grep -c '^>>> CRASH ForkCrashProbe.segvOnDemand')" "1"

echo
echo "PASS=$pass FAIL=$fail"
[ "$fail" -eq 0 ]
