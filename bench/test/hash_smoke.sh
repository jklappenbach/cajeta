#!/usr/bin/env bash
# hash/ smoke (Cajeta side; benchmark-reference unit 8, spec §6.5). Builds the
# release harness, runs `--area hash` under CAJETA_BENCH_SMOKE=1, and asserts
# every id on bench/hash/*.md emits a row in area `hash`, tagged `smoke`, with
# status ok and check_ok true (digests against the reference constants every
# competitor checks too). Anti-elision: an ok row of N hashes must take at
# least N/10 ns (the 256k rows count bytes/16: a 160 GB/s ceiling).
#
# Env: CAJETA (compiler), CAJETA_BENCH_OUT (directory for the CSV and log;
# default /tmp), NO_BUILD=1 (reuse build/bench).
set -euo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
source "$SCRIPT_DIR/csv_lib.sh"   # cawk / csv_fields: CSV-aware column checks
PROJ="$( cd -- "${SCRIPT_DIR}/.." &> /dev/null && pwd )"
REPO_ROOT="$( cd -- "${PROJ}/.." &> /dev/null && pwd )"
CAJETA="${CAJETA:-${REPO_ROOT}/build/src/cajeta}"
OUTDIR="${CAJETA_BENCH_OUT:-/tmp}"; mkdir -p "$OUTDIR"

fail() { echo "HASH FAIL: $1" >&2; exit 1; }
[[ -x "$CAJETA" ]] || fail "cajeta not found at $CAJETA"

cd "$PROJ"
if [[ "${NO_BUILD:-0}" != 1 ]]; then
    echo "[hash] building (release)..."
    t0=$SECONDS
    "$CAJETA" release >"$OUTDIR/hash-build.log" 2>&1 || { tail -30 "$OUTDIR/hash-build.log"; fail "release build failed"; }
    echo "[hash] built in $((SECONDS - t0))s"
fi

OUT="$OUTDIR/profile-hash.csv"
LOG="$OUTDIR/hash-run.log"
rm -f "$OUT"
t0=$SECONDS
CAJETA_BENCH_SMOKE=1 PROFILE_RUN_ID=hash-run "$PROJ/build/bench" --run --area hash --out "$OUT" >"$LOG" 2>&1 \
    || { grep -vE "example:" "$LOG" | tail -15; fail "--run exited non-zero"; }
echo "[hash] ran in $((SECONDS - t0))s"

while read -r b ops; do
    [[ -z "$b" ]] && continue
    row="$(cawk -v b="$b" 'NR>1 && $4==b {print; exit}' "$OUT")"
    [[ -n "$row" ]] || fail "no row for hash/$b"
    st="$(printf '%s' "$row" | cawk '{print $28}')"
    ar="$(printf '%s' "$row" | cawk '{print $5}')"
    [[ "$ar" == "hash" ]] || fail "$b area=$ar (want hash; the id is hash/$b)"
    printf '%s' "$row" | cawk '{print $30}' | grep -q 'smoke' || fail "$b row not tagged smoke"
    [[ "$st" == "ok" ]] || fail "$b status=$st notes=$(printf '%s' "$row" | cawk '{print $30}')"
    [[ "$(printf '%s' "$row" | cawk '{print $29}')" == "true" ]] || fail "$b check_ok not true"
    min="$(printf '%s' "$row" | cawk '{print $17}')"
    (( min >= ops / 10 )) || fail "$b min_ns=$min for $ops hashes looks elided"
    echo "[hash] $b ok  (input_size,min_ns)=$(printf '%s' "$row" | cawk '{print $9, $17}')"
done <<'IDS'
xxhash3 10000000
xxhash3-256k 163840000
xxhash3_128 10000000
xxhash3_128-256k 163840000
siphash 10000000
defaulthasher-string 10000000
defaulthasher-int 100000000
hash-struct 10000000
IDS

echo "HASH OK"
