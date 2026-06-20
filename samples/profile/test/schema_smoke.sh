#!/usr/bin/env bash
# Unit 2 TDD smoke test — written before the report code.
# Drives `profile --selftest --out <path>`, which writes a results CSV (+ a
# sibling .env CSV) using the real reporting schema + a real /proc peak-RSS read.
# Asserts: (1) header carries every schema column and exactly 30 columns;
# (2) the ok row's peak_rss_kb is a positive integer (real /proc/self/status read);
# (3) a status=skipped row is emitted; (4) env.csv captured a value read via
# System.env.get (proving the env capability works).
set -euo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
PROJ="$( cd -- "${SCRIPT_DIR}/.." &> /dev/null && pwd )"
REPO_ROOT="$( cd -- "${PROJ}/../.." &> /dev/null && pwd )"
CAJETA="${CAJETA:-${REPO_ROOT}/build-cajeta/src/cajeta}"

fail() { echo "SCHEMA FAIL: $1" >&2; exit 1; }
[[ -x "$CAJETA" ]] || fail "cajeta not found at $CAJETA"

cd "$PROJ"
echo "[schema] building..."
"$CAJETA" build >/dev/null || fail "build failed"

OUT="/tmp/profile-schema-test.csv"
ENV="${OUT}.env"
rm -f "$OUT" "$ENV"

echo "[schema] running selftest..."
PROFILE_RUN_TS=1750000000 PROFILE_RUN_ID=smoke-run "$PROJ/build/profile" --selftest --out "$OUT" || fail "selftest exited non-zero"

[[ -f "$OUT" ]] || fail "results CSV not written: $OUT"
HEADER="$(head -1 "$OUT")"
echo "[schema] header: $HEADER"

# (1) required columns present
for col in schema_version run_id timestamp_unix benchmark area language library flags \
           warmup trials min_ns median_ns mean_ns p95_ns throughput peak_rss_kb \
           alloc_bytes alloc_count working_set_kb status check_ok notes variant; do
    echo "$HEADER" | tr ',' '\n' | grep -qx "$col" || fail "header missing column: $col"
done

# (1) exactly 31 columns
NCOLS="$(head -1 "$OUT" | awk -F, '{print NF}')"
[[ "$NCOLS" == "31" ]] || fail "expected 31 columns, got $NCOLS"

# (2) ok row (line 2) peak_rss_kb (col 23) is a positive integer
PEAK="$(awk -F, 'NR==2{print $23}' "$OUT")"
[[ "$PEAK" =~ ^[0-9]+$ ]] || fail "peak_rss_kb not an integer: '$PEAK'"
[[ "$PEAK" -gt 0 ]] || fail "peak_rss_kb not positive: $PEAK"
echo "[schema] peak_rss_kb = $PEAK"

# (3) skipped row (line 3) status (col 28) == skipped
STATUS3="$(awk -F, 'NR==3{print $28}' "$OUT")"
[[ "$STATUS3" == "skipped" ]] || fail "expected skipped row, got status='$STATUS3'"

# (4) env.csv captured run_id + a System.env.get value
[[ -f "$ENV" ]] || fail "env CSV not written: $ENV"
grep -q "^run_id,smoke-run$" "$ENV" || fail "env.csv missing run_id"
grep -q "^timestamp_unix,1750000000$" "$ENV" || fail "env.csv missing System.env PROFILE_RUN_TS"

echo "SCHEMA OK"
