#!/usr/bin/env bash
# Unit 5 TDD smoke test (codec area, Cajeta side) — written before the codec
# benchmarks. Fetches the JSON corpus, runs the harness, and asserts:
#  - json-tokenize produces 3 ok rows (twitter/citm_catalog/canada) — status ok
#    means the Cajeta tokenizer hit the EXACT expected token count per file;
#  - input_size (file bytes) is recorded so the report can derive MB/s;
#  - base64-encode + base64-decode are ok (round-trip cross-checks);
#  - without PROFILE_DATA_DIR, json-tokenize records a skip (no crash).
set -euo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
PROJ="$( cd -- "${SCRIPT_DIR}/.." &> /dev/null && pwd )"
REPO_ROOT="$( cd -- "${PROJ}/../.." &> /dev/null && pwd )"
CAJETA="${CAJETA:-${REPO_ROOT}/build-cajeta/src/cajeta}"
DATA="/tmp/profile-codec-data"

fail() { echo "CODEC FAIL: $1" >&2; exit 1; }
[[ -x "$CAJETA" ]] || fail "cajeta not found at $CAJETA"

cd "$PROJ"
# Benchmarks are measured as native AOT --release (spec §1.4): the debug build
# keeps large temporaries on the stack and overflows on multi-MB buffers.
echo "[codec] building (release)..."
"$CAJETA" release >/dev/null || fail "release build failed"

# Network precheck (the corpus must be fetchable).
if ! timeout 12 curl -fsSL --max-time 10 -o /dev/null \
       "https://raw.githubusercontent.com/miloyip/nativejson-benchmark/master/data/twitter.json" 2>/dev/null; then
    echo "CODEC SKIP: network unreachable (JSON corpus unavailable)"
    exit 0
fi

echo "[codec] fetching corpus..."
PROFILE_DATA_DIR="$DATA" bash "$PROJ/datasets/fetch.sh" >/dev/null || fail "dataset fetch failed"

OUT="/tmp/profile-codec.csv"
rm -f "$OUT"
echo "[codec] running with corpus..."
PROFILE_RUN_ID=codec-run PROFILE_DATA_DIR="$DATA" "$PROJ/build/profile" --run --out "$OUT" >/tmp/codec-run.log 2>&1 \
    || { tail -20 /tmp/codec-run.log; fail "--run crashed"; }
grep -E "measured json-tokenize|measured base64" /tmp/codec-run.log | sed 's/^/  /'

col() { awk -F, -v b="$1" -v v="$2" -v c="$3" 'NR>1 && $4==b && $31==v {print $c; exit}' "$OUT"; }

# json-tokenize: 3 variants, each ok (exact token count matched) with input_size
for vrt in twitter citm_catalog canada; do
    st="$(col json-tokenize "$vrt" 28)"
    [[ "$st" == "ok" ]] || fail "json-tokenize/$vrt status=$st (token count mismatch?)"
    isz="$(col json-tokenize "$vrt" 9)"
    [[ "$isz" =~ ^[0-9]+$ && "$isz" -gt 0 ]] || fail "json-tokenize/$vrt input_size=$isz"
    mn="$(col json-tokenize "$vrt" 17)"
    [[ "$mn" =~ ^[0-9]+$ ]] || fail "json-tokenize/$vrt min_ns=$mn"
    echo "[codec] json-tokenize/$vrt ok  bytes=$isz  min_ns=$mn"
done

# json DOM-family: citm (integer-only) runs ok; twitter/canada skip (v1 DOM
# doesn't parse floats) — the limitation surfaced as data, not a crash.
for bench in json-dom json-serialize json-roundtrip; do
    st="$(col "$bench" citm_catalog 28)"
    [[ "$st" == "ok" ]] || fail "$bench/citm_catalog status=$st"
    mn="$(col "$bench" citm_catalog 17)"
    echo "[codec] $bench/citm_catalog ok  min_ns=$mn"
    for vrt in twitter canada; do
        st="$(col "$bench" "$vrt" 28)"
        [[ "$st" == "skipped" ]] || fail "$bench/$vrt expected skip (floats), got $st"
        echo "[codec] $bench/$vrt skipped (floats) ✓"
    done
done

# base64 enc/dec ok
for b in base64-encode base64-decode; do
    st="$(awk -F, -v b="$b" 'NR>1 && $4==b {print $28; exit}' "$OUT")"
    [[ "$st" == "ok" ]] || fail "$b status=$st"
    echo "[codec] $b ok"
done

# Without PROFILE_DATA_DIR, json-tokenize records a skip (no crash).
echo "[codec] no-dataset skip path..."
OUT2="/tmp/profile-codec-nodata.csv"; rm -f "$OUT2"
PROFILE_RUN_ID=nd "$PROJ/build/profile" --run --out "$OUT2" >/dev/null 2>&1 || fail "--run crashed without data dir"
ST="$(awk -F, 'NR>1 && $4=="json-tokenize" {print $28; exit}' "$OUT2")"
[[ "$ST" == "skipped" ]] || fail "json-tokenize should skip without dataset, got status=$ST"

echo "CODEC OK"
