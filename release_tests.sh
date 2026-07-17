#!/bin/bash
# Run the RELEASE regression subset — the curated set of cross-compilation-
# sensitive test suites listed in test/release_filter.txt, rather than the
# full ~1725-test battery (`cajeta_tests.sh` with no args). This is what the
# release workflow runs on every target: the full suite is too expensive to
# run on all four matrix platforms, and most of it (the host-independent
# front-end suites) can't regress due to cross-compilation anyway. See
# test/release_filter.txt for the suite list and the rationale.
#
# The actual sharded execution + crash-isolation reporting is reused from
# cajeta_tests.sh (this script just selects the subset and delegates), so a
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
#   PARALLEL=N   shard count (default: nproc, capped at 32 by cajeta_tests.sh).
#                PARALLEL=0 forces a serial run.
#
# Exit status is cajeta_tests.sh's: non-zero if any selected test fails or a
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

# The release gate certifies a *shippable toolchain*, not merely a green test
# binary. cajeta_test embeds its own JIT and is INDEPENDENT of the `cajeta`
# compiler CLI — so a tree where cajeta_test built but the `cajeta` compiler did
# NOT (failed to link, or a stale/partial build) still runs every test green,
# falsely certifying a release whose compiler is missing or broken. That hole is
# invisible under NO_BUILD=1, which skips build.sh and only checked cajeta_test.
# CI catches it via separate Build + Smoke steps, but a standalone
# `NO_BUILD=1 ./release_tests.sh` (exactly how this is run by hand) does not.
# Require the compiler binary AND smoke `--version`, so the gate fails if cajeta
# can't be made — in every invocation path.
CAJETA_BIN="build/src/cajeta"
if [ ! -x "$CAJETA_BIN" ]; then
    if [ -x "${CAJETA_BIN}.exe" ]; then          # mingw/Windows produces cajeta.exe
        CAJETA_BIN="${CAJETA_BIN}.exe"
    else
        echo "error: cajeta compiler not built at $CAJETA_BIN" >&2
        echo "       the release gate requires a working compiler, not just $TEST_BIN" >&2
        echo "       build it with ./build.sh (or drop NO_BUILD=1 to build here)" >&2
        exit 1
    fi
fi
if ! "$CAJETA_BIN" --version >/dev/null 2>&1; then
    echo "error: '$CAJETA_BIN --version' failed — the cajeta compiler is present but not runnable" >&2
    "$CAJETA_BIN" --version || true             # surface the actual error/exit
    exit 1
fi
echo ">> Compiler smoke: $("$CAJETA_BIN" --version 2>/dev/null | head -1)"

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
    # Surface WHY: the 2>/dev/null above hides load failures entirely (a
    # Windows exe the loader rejects — e.g. a multi-GB PE from a static-LLVM
    # -g link — dies before main with empty stdout AND useful stderr/exit
    # code). Re-run once without the gag and report size + exit status.
    ls -l "$TEST_BIN" "$TEST_BIN.exe" 2>/dev/null >&2 || true
    rc=0
    CAJETA_SOURCE_ROOT="$SCRIPT_DIR" "$TEST_BIN" --gtest_list_tests >/dev/null || rc=$?
    echo "       exit status: $rc" >&2
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

# Delegate to cajeta_tests.sh for sharded execution. PARALLEL is forced (default
# nproc) so the subset runs in parallel even though it's a filtered run —
# cajeta_tests.sh's parallel discovery honors these patterns. NO_BUILD avoids a
# redundant second build (we already built / verified above).
echo ">> Release subset: ${#patterns[@]} suites (delegating to cajeta_tests.sh, parallel)"
run_log="$(mktemp)"
trap 'rm -f "$run_log"' EXIT
set +e
env NO_BUILD=1 PARALLEL="${PARALLEL:-$(nproc 2>/dev/null || echo 4)}" \
    ./cajeta_tests.sh "${patterns[@]}" 2>&1 | tee "$run_log"
rc=${PIPESTATUS[0]}
set -e

[ "$rc" -eq 0 ] && exit 0

# Flake-tolerant gate. A handful of the JIT-driven concurrency suites (parallel
# streams, async/spawn, task drop) intermittently SIGSEGV/SIGABRT under the
# heavy thread oversubscription of N parallel shards each spawning their own
# worker pools on a small CI runner — they pass in isolation (tracked
# separately). Those surface as `Crashed`/`Timed out`, never as a `Failed`
# assertion. So: if the run had ANY real test failure, fail now (deterministic,
# never retried). Otherwise re-run JUST the crashed/timed-out tests serially
# (PARALLEL=0) — isolation reproduces the conditions under which they pass — and
# let the gate go green only if they then succeed. Capped: a large crash count
# is a real regression, not flakiness, so don't paper over it.
summary="$(grep -E '^Passed: [0-9]+ ' "$run_log" | tail -1)"
n_failed=$(sed -nE 's/.*Failed: ([0-9]+).*/\1/p' <<< "$summary")
n_to=$(sed -nE 's/.*Timed out: ([0-9]+).*/\1/p' <<< "$summary")
n_crash=$(sed -nE 's/.*Crashed: ([0-9]+).*/\1/p' <<< "$summary")
n_failed=${n_failed:-0}; n_to=${n_to:-0}; n_crash=${n_crash:-0}

if [ "$n_failed" -gt 0 ]; then
    echo ">> Release gate: $n_failed deterministic test failure(s) — not retrying." >&2
    exit "$rc"
fi

flaky_max=12
n_flaky=$(( n_to + n_crash ))
if [ "$n_flaky" -eq 0 ] || [ "$n_flaky" -gt "$flaky_max" ]; then
    echo ">> Release gate: $n_flaky crashed/timed-out test(s) (cap $flaky_max) — treating as a real failure, not retrying." >&2
    exit "$rc"
fi

# Pull the offending test ids from the `Crashed:` / `Timed out` report sections
# (each entry is `  Suite.test (...)`); skip the instructional `Re-run ...` line.
# Indexed-array read loop rather than `mapfile -t` — macOS ships bash 3.2,
# which has no mapfile/readarray. Process substitution is fine in 3.2.
retry=()
while IFS= read -r _rt; do
    [ -n "$_rt" ] && retry+=("$_rt")
done < <(awk '
    /^Crashed:/        { mode="x"; next }
    /^Timed out/       { mode="x"; next }
    /^Failed tests:/   { mode="";  next }
    /^Re-run a crash/  { mode="";  next }
    /^[[:space:]]*$/   { mode="" }
    mode=="x" && $1 ~ /^[A-Za-z_][A-Za-z0-9_]*\.[A-Za-z0-9_]+$/ { print $1 }
' "$run_log" | sort -u)

if [ "${#retry[@]}" -eq 0 ]; then
    echo ">> Release gate: could not parse crashed/timed-out test ids to retry." >&2
    exit "$rc"
fi

echo
echo ">> Release gate: retrying ${#retry[@]} crashed/timed-out test(s) serially in isolation:"
printf '   %s\n' "${retry[@]}"
exec env NO_BUILD=1 PARALLEL=0 ./cajeta_tests.sh "${retry[@]}"
