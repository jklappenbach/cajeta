#!/usr/bin/env bash
# Driver smoke (profile Unit 17, moved to bench/; benchmark-reference 2.2.3).
# Runs bench.sh in short mode and asserts:
#  (1) --list prints benchmarks under their catalog paths;
#  (2) --area math runs only the math package, writing results.csv + env.csv,
#      no report site (the Astro site renders results now);
#  (3) --bench selects a single benchmark;
#  (4) --machine reaches env.csv as `machine,<name>`;
#  (5) `publish` is the unit-22 stub: prints so and exits 2.
set -euo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
PROJ="$( cd -- "${SCRIPT_DIR}/.." &> /dev/null && pwd )"

fail() { echo "DRIVER FAIL: $1" >&2; exit 1; }
cd "$PROJ"
export CAJETA_BENCH_SMOKE=1

# (1) --list works (build + list, no run)
LIST="$(bash bench.sh --list 2>/dev/null)" || fail "bench.sh --list failed"
echo "$LIST" | grep -q "saxpy \[lang/saxpy\]" || fail "--list missing saxpy [lang/saxpy]"
echo "$LIST" | grep -q "matmul \[math/matmul\]" || fail "--list missing matmul [math/matmul]"

# (2) --area filter runs only that package, full pipeline
OUT="/tmp/cajeta-bench-driver-test"
rm -rf "$OUT"
PROFILE_OUT="$OUT" bash bench.sh --machine smokebox --area math >/tmp/cajeta-bench-driver.log 2>&1 \
    || { tail -15 /tmp/cajeta-bench-driver.log; fail "bench.sh --area math failed"; }
[[ -f "$OUT/results.csv" ]] || fail "results.csv not written"
[[ -f "$OUT/env.csv" ]]     || fail "env.csv not written"
[[ -e "$OUT/site" || -e "$OUT/report.md" ]] && fail "a report was generated (report.py is retired)"
NONMATH="$(awk -F, 'NR>1 && $5 !~ /^math\//' "$OUT/results.csv" | wc -l)"
[[ "$NONMATH" == "0" ]] || fail "expected only math/ rows, got $NONMATH others"
awk -F, 'NR>1 && $4=="matmul" && $5=="math/matmul" && $28=="ok"' "$OUT/results.csv" | grep -q . || fail "math/matmul not ok in filtered run"

# (4) --machine reached env.csv
grep -qx "machine,smokebox" "$OUT/env.csv" || fail "env.csv missing machine,smokebox"
grep -qx "smoke,1" "$OUT/env.csv" || fail "env.csv missing the smoke marker"

# (3) --bench selects a single benchmark
OUT2="/tmp/cajeta-bench-driver-test2"; rm -rf "$OUT2"
PROFILE_OUT="$OUT2" bash bench.sh --bench dot-product >/dev/null 2>&1 || fail "bench.sh --bench failed"
N="$(awk -F, 'NR>1' "$OUT2/results.csv" | wc -l)"
[[ "$N" == "1" ]] || fail "--bench dot-product expected 1 row, got $N"

# (5) publish stub
set +e
MSG="$(bash bench.sh publish phoenix 20261101T0300Z-0.46.3 2>&1)"; RC=$?
set -e
[[ "$RC" == "2" ]] || fail "publish stub exit $RC, want 2"
echo "$MSG" | grep -q "implemented in unit 22" || fail "publish stub message: $MSG"

echo "DRIVER OK"
