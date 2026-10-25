#!/usr/bin/env bash
# Harness lifecycle smoke (profile Unit 1, moved to bench/; on dev.cajeta.bench
# since benchmark-reference unit 2). Asserts:
# (1) `cajeta release` produces build/bench; with no arguments it prints the
#     usage and exits 2 (BenchMain has no run-everything default);
# (2) a measured run drives setup once first, the workload for warmup (1) +
#     trials (3) in short mode, checkResult once, and teardown once, last —
#     on the selftest/ scaffold, registered only under CAJETA_BENCH_SCAFFOLD=1;
# (3) `--list` prints each benchmark as its id <package>/<slug>, and the scaffold is
#     absent from the catalog listing without CAJETA_BENCH_SCAFFOLD;
# (4) bench/registered-ids.txt is ./bench.sh --registered-ids, id for id.
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

# (1) build → build/bench; no arguments → usage, exit 2
echo "[smoke] building..."
"$CAJETA" release >/dev/null || fail "cajeta release build failed"
[[ -x "$PROJ/build/bench" ]] || fail "expected binary build/bench not produced"
set +e
USAGE="$("$PROJ/build/bench" 2>&1)"; RC=$?
set -e
[[ "$RC" == "2" ]] || fail "no-argument run exited $RC, want 2 (usage)"
printf '%s\n' "$USAGE" | grep -q "^usage:" || fail "no-argument run printed no usage"

# (2) lifecycle order in a measured short-mode run
echo "[smoke] running..."
D="${CAJETA_BENCH_OUT:-/tmp}/cajeta-bench-smoke"; mkdir -p "$D"
OUT="$(CAJETA_BENCH_SCAFFOLD=1 CAJETA_BENCH_SMOKE=1 "$PROJ/build/bench" --run --bench example --out "$D/results.csv")" \
    || fail "build/bench --run --bench example exited non-zero"
ORDER="$(printf '%s\n' "$OUT" | grep -oE 'example: (setup|run|teardown|checkResult)' | sed 's/example: //' | tr '\n' ',')"
echo "[smoke] lifecycle: $ORDER"
[[ "$ORDER" == setup,* ]]       || fail "setup is not first: [$ORDER]"
[[ "$ORDER" == *,teardown, ]]   || fail "teardown is not last: [$ORDER]"
[[ "$(printf '%s' "$ORDER" | grep -o 'setup' | wc -l)" == "1" ]]       || fail "setup ran more than once: [$ORDER]"
[[ "$(printf '%s' "$ORDER" | grep -o 'teardown' | wc -l)" == "1" ]]    || fail "teardown ran more than once: [$ORDER]"
[[ "$(printf '%s' "$ORDER" | grep -o 'checkResult' | wc -l)" == "1" ]] || fail "checkResult did not run exactly once: [$ORDER]"
[[ "$(printf '%s' "$ORDER" | grep -o 'run' | wc -l)" -ge 4 ]]          || fail "fewer than warmup 1 + trials 3 runs: [$ORDER]"

# (3) --list prints <area>/<name>; scaffold only under CAJETA_BENCH_SCAFFOLD
LIST="$(CAJETA_BENCH_SCAFFOLD=1 "$PROJ/build/bench" --list)" || fail "--list exited non-zero"
printf '%s\n' "$LIST" | grep -qE "^  selftest/example  " || fail "--list missing selftest/example"
CAT="$("$PROJ/build/bench" --list)" || fail "--list exited non-zero"
printf '%s\n' "$CAT" | grep -q "selftest/" && fail "scaffold benchmarks listed without CAJETA_BENCH_SCAFFOLD"
printf '%s\n' "$CAT" | grep -qE "^  collection/hashmap-int  " || fail "--list missing collection/hashmap-int"
printf '%s\n' "$CAT" | grep -qE "^  [a-z]+/([a-z0-9_.-]+)/\1  " && fail "a --list line doubles its name (area must be the package only)"

# (4) bench/registered-ids.txt (the site's registered-id hook) is what the
#     builds register: ./bench.sh --registered-ids, the union of every build's
#     --list, id for id.
REG="$(CAJETA="$CAJETA" bash "$PROJ/bench.sh" --registered-ids 2>/dev/null | awk '{print $1}')" || fail "bench.sh --registered-ids failed"
FILE_IDS="$(grep -vE '^\s*(#|$)' "$PROJ/registered-ids.txt" | awk '{print $1}')"
[[ "$REG" == "$FILE_IDS" ]] || fail "registered-ids.txt is stale against the builds: $(diff <(printf '%s\n' "$FILE_IDS") <(printf '%s\n' "$REG") | grep '^[<>]' | head -10 | tr '\n' ' ')"
echo "[smoke] registered-ids.txt matches the builds ($(printf '%s\n' "$REG" | wc -l) ids)"

echo "SMOKE OK"
