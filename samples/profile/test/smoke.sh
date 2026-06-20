#!/usr/bin/env bash
# Unit 1 TDD smoke test for samples/profile — written before the scaffold.
# Asserts: (1) cajeta build produces build/profile and it runs with a banner;
# (2) Profile invokes a Benchmark's setup→run→teardown→checkResult in order, once;
# (3) `--list` prints each benchmark's name + area.
#
# Override the compiler with CAJETA=/path/to/cajeta.
set -euo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
PROJ="$( cd -- "${SCRIPT_DIR}/.." &> /dev/null && pwd )"
REPO_ROOT="$( cd -- "${PROJ}/../.." &> /dev/null && pwd )"
CAJETA="${CAJETA:-${REPO_ROOT}/build-cajeta/src/cajeta}"

fail() { echo "SMOKE FAIL: $1" >&2; exit 1; }

[[ -x "$CAJETA" ]] || fail "cajeta build tool not found at $CAJETA (build the compiler: ./build.sh)"

cd "$PROJ"

# (1) build → build/profile
echo "[smoke] building..."
"$CAJETA" build || fail "cajeta build failed"
[[ -x "$PROJ/build/profile" ]] || fail "expected binary build/profile not produced"

# (1) run → banner
echo "[smoke] running..."
OUT="$("$PROJ/build/profile")" || fail "build/profile exited non-zero"
printf '%s\n' "$OUT"
printf '%s\n' "$OUT" | grep -q "Cajeta profile" || fail "missing banner line"

# (2) lifecycle order: each phase exactly once, in order
ORDER="$(printf '%s\n' "$OUT" | grep -oE 'example: (setup|run|teardown|checkResult)' | sed 's/example: //')"
EXPECTED=$'setup\nrun\nteardown\ncheckResult'
[[ "$ORDER" == "$EXPECTED" ]] || fail "lifecycle order wrong; expected [setup,run,teardown,checkResult] got [$(echo "$ORDER" | tr '\n' ',')]"

# (3) --list prints name + area
LIST="$("$PROJ/build/profile" --list)" || fail "--list exited non-zero"
printf '%s\n' "$LIST"
printf '%s\n' "$LIST" | grep -q "example \[scaffold\]" || fail "--list missing 'example [scaffold]'"

echo "SMOKE OK"
