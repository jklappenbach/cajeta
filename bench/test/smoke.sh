#!/usr/bin/env bash
# Harness lifecycle smoke (profile Unit 1, moved to bench/). Asserts:
# (1) `cajeta release` produces build/bench and it runs with a banner;
# (2) the entry drives a Benchmark's setup→run→teardown→checkResult in order,
#     once (the selftest/ scaffold, registered under CAJETA_BENCH_SCAFFOLD=1);
# (3) `--list` prints each benchmark's name + catalog path, and the scaffold
#     is absent from the catalog listing without CAJETA_BENCH_SCAFFOLD.
#
# Override the compiler with CAJETA=/path/to/cajeta.
set -euo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
PROJ="$( cd -- "${SCRIPT_DIR}/.." &> /dev/null && pwd )"
REPO_ROOT="$( cd -- "${PROJ}/.." &> /dev/null && pwd )"
CAJETA="${CAJETA:-${REPO_ROOT}/build/src/cajeta}"

fail() { echo "SMOKE FAIL: $1" >&2; exit 1; }

[[ -x "$CAJETA" ]] || fail "cajeta build tool not found at $CAJETA (build the compiler: ./build.sh)"

cd "$PROJ"

# (1) build → build/bench
echo "[smoke] building..."
"$CAJETA" release >/dev/null || fail "cajeta release build failed"  # default mode executes all benchmarks → --release (§1.4)
[[ -x "$PROJ/build/bench" ]] || fail "expected binary build/bench not produced"

# (1) run → banner; (2) lifecycle order: each phase exactly once, in order.
# The default (no --run) mode drives the selected benchmarks once each.
echo "[smoke] running..."
OUT="$(CAJETA_BENCH_SCAFFOLD=1 "$PROJ/build/bench" --bench example)" || fail "build/bench exited non-zero"
printf '%s\n' "$OUT"
printf '%s\n' "$OUT" | grep -q "Cajeta bench" || fail "missing banner line"
ORDER="$(printf '%s\n' "$OUT" | grep -oE 'example: (setup|run|teardown|checkResult)' | sed 's/example: //')"
EXPECTED=$'setup\nrun\nteardown\ncheckResult'
[[ "$ORDER" == "$EXPECTED" ]] || fail "lifecycle order wrong; expected [setup,run,teardown,checkResult] got [$(echo "$ORDER" | tr '\n' ',')]"

# (3) --list prints name + catalog path; scaffold only under CAJETA_BENCH_SCAFFOLD
LIST="$(CAJETA_BENCH_SCAFFOLD=1 "$PROJ/build/bench" --list)" || fail "--list exited non-zero"
printf '%s\n' "$LIST" | grep -q "example \[selftest/example\]" || fail "--list missing 'example [selftest/example]'"
CAT="$("$PROJ/build/bench" --list)" || fail "--list exited non-zero"
printf '%s\n' "$CAT" | grep -q "selftest/" && fail "scaffold benchmarks listed without CAJETA_BENCH_SCAFFOLD"
printf '%s\n' "$CAT" | grep -q "hashmap-int \[collection/hashmap-int\]" || fail "--list missing 'hashmap-int [collection/hashmap-int]'"

echo "SMOKE OK"
