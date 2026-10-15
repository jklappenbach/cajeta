#!/bin/bash
# Tests for cajeta_tests.sh's prime-once-then-fan-out step (compile-cache 5.1.2).
#
# Runs the REAL parallel sweep on two small suites against a scratch prime
# cache and reads what the harness and the shards report:
#   cold cache : one prime step compiles and stores the stdlib object, and
#                every shard that follows links it (object HIT, never MISS);
#   warm cache : the prime step reports the object served;
#   cache off  : no prime step runs and the shards compile uncached.
# Needs a built test binary (NO_BUILD=1 is passed, so nothing is compiled
# under the run); skips itself without one. bash-3.2 compatible.

set -u
HARNESS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HARNESS_DIR/../.." && pwd)"
CT="$ROOT/cajeta_tests.sh"
TEST_BIN="${TEST_BIN:-$ROOT/build/test/cajeta_test}"
TMP="$ROOT/tmp/prime_once_test.$$"
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

pass=0; fail=0
ok()   { pass=$((pass+1)); printf '  ok   %s\n' "$1"; }
bad()  { fail=$((fail+1)); printf '  FAIL %s\n' "$1"; [ -n "${2:-}" ] && printf '        %s\n' "$2"; }
check(){ if [ "$2" = "$3" ]; then ok "$1"; else bad "$1" "expected [$3] got [$2]"; fi; }

if [ ! -x "$TEST_BIN" ]; then
    echo "prime_once_test: no built binary at $TEST_BIN, skipping"
    echo "PASS=0 FAIL=0 (skipped)"
    exit 0
fi

CACHE="$TMP/cache"
SUITES="SharedStdlibDefaultTests ClosureTransferCaptureTests"

# run_sweep <logdir> [env assignments...]: the parallel sweep over $SUITES
# with the shard logs kept in <logdir>; echoes the harness's own output.
run_sweep() {
    local logs="$1"; shift
    # shellcheck disable=SC2086
    env "$@" NO_BUILD=1 PARALLEL=2 TUI=0 PROGRESS=0 KEEP_LOGS="$logs" \
        CAJETA_PRIME_CACHE_DIR="$CACHE" CAJETA_PRIME_TIMING=1 \
        "$CT" $SUITES 2>&1
}

# --- 5.1.2 cold: one prime step, every shard hits ---------------------------
OUT="$(run_sweep "$TMP/cold")"
primes=$(printf '%s\n' "$OUT" | grep -c '^>> Stdlib prime: ')
check "5.1.2 cold sweep runs exactly one prime step" "$primes" "1"
cold=$(printf '%s\n' "$OUT" | grep -c '^>> Stdlib prime: cold')
check "5.1.2 the prime step on a cold cache compiles and stores" "$cold" "1"
objects=$(find "$CACHE" -name '*.o' 2>/dev/null | grep -c .)
check "5.1.2 one stdlib object is stored" "$objects" "1"
nlogs=$(ls "$TMP"/cold/shard_*.out 2>/dev/null | grep -c .)
check "5.1.2 two shards ran" "$nlogs" "2"
hits=$(cat "$TMP"/cold/shard_*.out "$TMP"/cold/srv_*.err 2>/dev/null | grep -c '\[shared-stdlib\] object HIT')
check "5.1.2 every shard links the stored object" "$hits" "2"
misses=$(cat "$TMP"/cold/shard_*.out "$TMP"/cold/srv_*.err 2>/dev/null | grep -c '\[shared-stdlib\] object MISS')
check "5.1.2 no shard compiles the stdlib cold" "$misses" "0"
passed=$(printf '%s\n' "$OUT" | sed -nE 's/^Passed: ([0-9]+) .*/\1/p')
check "5.1.2 the sweep's tests pass" "$(printf '%s\n' "$OUT" | grep -c 'Failed: 0 ')" "1"

# --- 5.1.2 warm: the prime step is served --------------------------------------
OUT="$(run_sweep "$TMP/warm")"
warm=$(printf '%s\n' "$OUT" | grep -c '^>> Stdlib prime: warm')
check "5.1.2 the prime step on a warm cache is served" "$warm" "1"
hits=$(cat "$TMP"/warm/shard_*.out "$TMP"/warm/srv_*.err 2>/dev/null | grep -c '\[shared-stdlib\] object HIT')
check "5.1.2 every shard still links the stored object" "$hits" "2"
check "5.1.2 the warm sweep passes the same count" \
      "$(printf '%s\n' "$OUT" | sed -nE 's/^Passed: ([0-9]+) .*/\1/p')" "$passed"

# --- 5.1.2 off: no prime step, shards uncached ---------------------------------
OUT="$(run_sweep "$TMP/off" CAJETA_PRIME_CACHE=0)"
primes=$(printf '%s\n' "$OUT" | grep -c '^>> Stdlib prime: ')
check "5.1.2 CAJETA_PRIME_CACHE=0 skips the prime step" "$primes" "0"
uncached=$(cat "$TMP"/off/shard_*.out "$TMP"/off/srv_*.err 2>/dev/null | grep -c '\[shared-stdlib\] object UNCACHED')
check "5.1.2 the shards compile uncached" "$uncached" "2"

echo
echo "PASS=$pass FAIL=$fail"
[ "$fail" -eq 0 ]
