#!/usr/bin/env bash
# Results-schema smoke (profile Unit 2, moved to bench/; schema v2 from
# dev.cajeta.bench since benchmark-reference unit 2). Asserts:
# (1) the header is schema v2: 37 columns, v1's 32 in order (backend at 32)
#     then machine, os, driver, model, platform;
# (2) an ok row carries schema_version 2, a positive peak_rss_kb (a real /proc
#     read), the machine from CAJETA_BENCH_MACHINE and platform cajeta;
# (3) a benchmark whose dataset is absent yields a status=skipped row with its
#     reason (never a silent omission);
# (4) env.csv lands beside results.csv with the run id, timestamp and machine.
set -euo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
PROJ="$( cd -- "${SCRIPT_DIR}/.." &> /dev/null && pwd )"
REPO_ROOT="$( cd -- "${PROJ}/.." &> /dev/null && pwd )"
CAJETA="${CAJETA:-${REPO_ROOT}/build/src/cajeta}"

fail() { echo "SCHEMA FAIL: $1" >&2; exit 1; }
[[ -x "$CAJETA" ]] || fail "cajeta not found at $CAJETA"

cd "$PROJ"
echo "[schema] building..."
"$CAJETA" release >/dev/null || fail "release build failed"

D="/tmp/cajeta-bench-schema"; rm -rf "$D"; mkdir -p "$D"
OUT="$D/results.csv"; ENV="$D/env.csv"
CAJETA_BENCH_SCAFFOLD=1 CAJETA_BENCH_MACHINE=schemabox CAJETA_BENCH_RUN_ID=smoke-run CAJETA_BENCH_RUN_TS=1750000000 \
    "$PROJ/build/bench" --run --bench sum --out "$OUT" >/dev/null || fail "--run exited non-zero"
[[ -f "$OUT" ]] || fail "results CSV not written: $OUT"
HEADER="$(head -1 "$OUT")"
echo "[schema] header: $HEADER"

# (1) v2 header
for col in schema_version run_id timestamp_unix benchmark area language library flags \
           warmup trials min_ns median_ns mean_ns p95_ns throughput peak_rss_kb \
           alloc_bytes alloc_count working_set_kb status check_ok notes variant backend \
           machine os driver model platform; do
    echo "$HEADER" | tr ',' '\n' | grep -qx "$col" || fail "header missing column: $col"
done
NCOLS="$(echo "$HEADER" | awk -F, '{print NF}')"
[[ "$NCOLS" == "37" ]] || fail "expected 37 columns, got $NCOLS"
[[ "$(echo "$HEADER" | awk -F, '{print $32}')" == "backend" ]]  || fail "column 32 is not backend"
[[ "$(echo "$HEADER" | awk -F, '{print $33}')" == "machine" ]]  || fail "column 33 is not machine"
[[ "$(echo "$HEADER" | awk -F, '{print $37}')" == "platform" ]] || fail "column 37 is not platform"

# (2) the ok row
ROW="$(awk -F, 'NR>1 && $4=="sum"{print; exit}' "$OUT")"
[[ -n "$ROW" ]] || fail "no sum row"
get() { echo "$ROW" | awk -F, -v c="$1" '{print $c}'; }
[[ "$(get 1)" == "2" ]]           || fail "schema_version $(get 1), want 2"
[[ "$(get 28)" == "ok" ]]         || fail "sum status $(get 28)"
PEAK="$(get 23)"
[[ "$PEAK" =~ ^[0-9]+$ && "$PEAK" -gt 0 ]] || fail "peak_rss_kb not a positive integer: '$PEAK'"
[[ "$(get 33)" == "schemabox" ]]  || fail "machine '$(get 33)', want schemabox"
[[ "$(get 37)" == "cajeta" ]]     || fail "platform '$(get 37)', want cajeta"
[[ -n "$(get 34)" ]]              || fail "os column empty"
echo "[schema] peak_rss_kb=$PEAK machine=$(get 33) os=$(get 34)"

# (3) skipped row: json-tokenize without PROFILE_DATA_DIR
OUT2="$D/skip.csv"
env -u PROFILE_DATA_DIR "$PROJ/build/bench" --run --bench json-tokenize --out "$OUT2" >/dev/null || fail "skip run exited non-zero"
ROW="$(awk -F, 'NR>1 && $4=="json-tokenize"{print; exit}' "$OUT2")"
[[ "$(get 28)" == "skipped" ]] || fail "expected skipped row, got status='$(get 28)'"
[[ -n "$(get 30)" ]]           || fail "skipped row carries no reason"

# (4) env.csv beside results.csv
[[ -f "$ENV" ]] || fail "env CSV not written: $ENV"
grep -q "^run_id,smoke-run$" "$ENV"            || fail "env.csv missing run_id"
grep -q "^timestamp_unix,1750000000$" "$ENV"   || fail "env.csv missing timestamp_unix"
grep -q "^machine,schemabox$" "$ENV"           || fail "env.csv missing machine"

echo "SCHEMA OK"
