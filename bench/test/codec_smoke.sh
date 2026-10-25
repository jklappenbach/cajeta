#!/usr/bin/env bash
# codec/ smoke (benchmark-reference unit 9, Cajeta side). Every id on the
# codec pages (bench/codec/json/Json.md, csv/Csv.md, Base64.md, Base32.md)
# must emit a row under CAJETA_BENCH_SMOKE=1 with status ok and check_ok true,
# with three exceptions that are measured stdlib gaps, not harness faults, and
# are asserted as such (status invalid, check_ok false, the reason in notes):
#   json-conformance        Json.parse accepts 9 of the 31 JSON_checker fail
#                           files (trailing content, a leading zero, raw control
#                           characters in strings, malformed exponents);
#   json-roundtrip/canada   Json.toBytes writes a float with at most 6 fraction
#                           digits, so canada's coordinates do not round-trip.
# A gap that closes (the row turns ok) passes and is reported; any other
# non-ok row fails. Deflate's four rows live in cajeta-codec/bench (its own
# bench_smoke.sh).
#
# Inputs: the nativejson corpus and the JSON_checker set from
# datasets/fetch.sh (sha256-checked), the CSV file from datasets/gen_csv.py
# (the 4 MB smoke scale; the 100 MB file is the full run's).
# Env: CAJETA (compiler), PROFILE_DATA_DIR (default /tmp/profile-codec-data),
# CAJETA_BENCH_OUT (where the CSVs land; default /tmp), NO_BUILD=1 (reuse
# build/bench).
set -euo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
PROJ="$( cd -- "${SCRIPT_DIR}/.." &> /dev/null && pwd )"
REPO_ROOT="$( cd -- "${PROJ}/.." &> /dev/null && pwd )"
CAJETA="${CAJETA:-${REPO_ROOT}/build/src/cajeta}"
DATA="${PROFILE_DATA_DIR:-/tmp/profile-codec-data}"
OUTDIR="${CAJETA_BENCH_OUT:-/tmp}"
PY="$(command -v python3)"

fail() { echo "CODEC FAIL: $1" >&2; exit 1; }
[[ -x "$CAJETA" ]] || fail "cajeta not found at $CAJETA"
mkdir -p "$OUTDIR"

cd "$PROJ"
# Measured as native AOT --release (spec §1.4): the debug build keeps large
# temporaries on the stack and overflows on multi-MB buffers.
if [[ -n "${NO_BUILD:-}" && -x "$PROJ/build/bench" ]]; then
    echo "[codec] reusing build/bench (NO_BUILD)"
else
    echo "[codec] building (release)..."
    t0=$SECONDS
    "$CAJETA" release >/dev/null || fail "release build failed"
    echo "[codec] build $((SECONDS - t0)) s"
fi

if ! timeout 12 curl -fsSL --max-time 10 -o /dev/null \
       "https://raw.githubusercontent.com/miloyip/nativejson-benchmark/master/data/twitter.json" 2>/dev/null; then
    echo "CODEC SKIP: network unreachable (JSON corpus unavailable)"
    exit 0
fi

echo "[codec] fetching corpus + JSON_checker set, generating the smoke CSV..."
PROFILE_DATA_DIR="$DATA" bash "$PROJ/datasets/fetch.sh" >/dev/null || fail "dataset fetch failed"
"$PY" "$PROJ/datasets/gen_csv.py" "$DATA" smoke >/dev/null || fail "gen_csv.py smoke failed"

OUT="$OUTDIR/profile-codec.csv"
LOG="$OUTDIR/codec-run.log"
rm -f "$OUT"
echo "[codec] running --area codec (smoke)..."
t0=$SECONDS
CAJETA_BENCH_SMOKE=1 PROFILE_RUN_ID=codec-run PROFILE_DATA_DIR="$DATA" \
    "$PROJ/build/bench" --run --area codec --out "$OUT" >"$LOG" 2>&1 || true
echo "[codec] run $((SECONDS - t0)) s"
[[ -s "$OUT" ]] || { tail -20 "$LOG"; fail "--run wrote no CSV"; }

# columns: 4 benchmark, 9 input_size, 17 min_ns, 18 median_ns, 28 status,
# 29 check_ok, 30 notes, 31 variant
row() { awk -F, -v b="$1" -v v="$2" 'NR>1 && $4==b && $31==v {print; exit}' "$OUT"; }
col() { row "$1" "$2" | awk -F, -v c="$3" '{print $c}'; }

# The known gaps: id|variant -> a substring the notes must carry.
declare -A GAP=(
    ["json-conformance|"]="misclassified"
    ["json-roundtrip|canada"]="NOT equal after round trip"
)

check_id() {   # bench variant
    local b="$1" v="$2" r st ck nt mn md isz
    r="$(row "$b" "$v")"
    [[ -n "$r" ]] || fail "$b/${v:-.} emitted no row"
    st="$(col "$b" "$v" 28)"; ck="$(col "$b" "$v" 29)"; nt="$(col "$b" "$v" 30)"
    mn="$(col "$b" "$v" 17)"; md="$(col "$b" "$v" 18)"; isz="$(col "$b" "$v" 9)"
    local key="$b|$v"
    if [[ "$st" == "ok" && "$ck" == "true" ]]; then
        [[ "$mn" =~ ^[0-9]+$ && "$md" =~ ^[0-9]+$ && "$md" -gt 0 ]] || fail "$b/${v:-.} median_ns=$md (elided?)"
        [[ "$isz" =~ ^[0-9]+$ && "$isz" -gt 0 ]] || fail "$b/${v:-.} input_size=$isz"
        if [[ -n "${GAP[$key]:-}" ]]; then
            echo "[codec] $b/${v:-.} ok  (known gap CLOSED: update this smoke and the report)"
        else
            echo "[codec] $b/${v:-.} ok  median_ns=$md  ${nt}"
        fi
    elif [[ -n "${GAP[$key]:-}" && "$st" == "invalid" && "$ck" == "false" && "$nt" == *"${GAP[$key]}"* ]]; then
        echo "[codec] $b/${v:-.} KNOWN GAP (invalid): $nt"
    else
        fail "$b/${v:-.} status=$st check_ok=$ck notes=$nt"
    fi
}

for b in json-tokenize json-bind-skip json-dom json-serialize json-roundtrip; do
    for v in twitter citm_catalog canada; do check_id "$b" "$v"; done
done
for b in json-conformance csv-parse csv-write base64-encode base64-decode base32-encode base32-decode; do
    check_id "$b" ""
done

# The CSV rows ran on the smoke file and say so.
for b in csv-parse csv-write; do
    [[ "$(col "$b" "" 30)" == *"smoke input 4 MB"* ]] || fail "$b notes do not record the smoke input size"
done

# Every row carries its package and the smoke tag.
bad="$(awk -F, 'NR>1 && ($5 != "codec" || $30 !~ /smoke/)' "$OUT" | wc -l)"
[[ "$bad" == "0" ]] || fail "$bad codec rows without area codec or the smoke tag"
n="$(awk -F, 'NR>1' "$OUT" | wc -l)"
[[ "$n" == "22" ]] || fail "expected 22 codec rows (15 json-per-file + conformance + 2 csv + 4 base-n), got $n"

# Without PROFILE_DATA_DIR the dataset benchmarks record skips (no crash).
echo "[codec] no-dataset skip path..."
OUT2="$OUTDIR/profile-codec-nodata.csv"; rm -f "$OUT2"
CAJETA_BENCH_SMOKE=1 PROFILE_RUN_ID=nd env -u PROFILE_DATA_DIR "$PROJ/build/bench" --run --area codec --out "$OUT2" >/dev/null 2>&1 \
    || fail "--run without a data dir exited non-zero"
for b in json-tokenize json-conformance csv-parse csv-write; do
    st="$(awk -F, -v b="$b" 'NR>1 && $4==b {print $28; exit}' "$OUT2")"
    [[ "$st" == "skipped" ]] || fail "$b should skip without a dataset, got status=$st"
done

echo "CODEC OK"
