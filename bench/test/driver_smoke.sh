#!/usr/bin/env bash
# Unit 17 TDD smoke test (top-level driver + selection). Runs bench.sh with an
# --area filter (math — fast, no dataset) and asserts the full pipeline produced
# results.csv (math rows only), env.csv, and the report site; plus --list works
# and --bench selects a single benchmark.
set -euo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
PROJ="$( cd -- "${SCRIPT_DIR}/.." &> /dev/null && pwd )"

fail() { echo "DRIVER FAIL: $1" >&2; exit 1; }
cd "$PROJ"

# (1) --list works (build + list, no run)
LIST="$(bash bench.sh --list 2>/dev/null)" || fail "bench.sh --list failed"
echo "$LIST" | grep -q "saxpy \[math\]" || fail "--list missing saxpy [math]"

# (2) --area filter runs only that area, full pipeline
OUT="/tmp/profile-driver-test"
rm -rf "$OUT"
PROFILE_OUT="$OUT" bash bench.sh --area math >/tmp/driver.log 2>&1 || { tail -15 /tmp/driver.log; fail "bench.sh --area math failed"; }

[[ -f "$OUT/results.csv" ]] || fail "results.csv not written"
[[ -f "$OUT/env.csv" ]]     || fail "env.csv not written"
[[ -f "$OUT/site/index.html" ]] || fail "report site not generated"

# only math rows (+ header)
AREAS="$(awk -F, 'NR>1{print $5}' "$OUT/results.csv" | sort -u | tr '\n' ' ')"
[[ "$AREAS" == "math " ]] || fail "expected only math rows, got areas: [$AREAS]"
awk -F, 'NR>1 && $4=="saxpy" && $28=="ok"' "$OUT/results.csv" | grep -q saxpy || fail "saxpy not ok in filtered run"

# (3) --bench selects a single benchmark
OUT2="/tmp/profile-driver-test2"; rm -rf "$OUT2"
PROFILE_OUT="$OUT2" bash bench.sh --bench dot-product >/dev/null 2>&1 || fail "bench.sh --bench failed"
N="$(awk -F, 'NR>1' "$OUT2/results.csv" | wc -l)"
[[ "$N" == "1" ]] || fail "--bench dot-product expected 1 row, got $N"

echo "DRIVER OK"
