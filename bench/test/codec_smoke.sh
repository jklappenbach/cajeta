#!/usr/bin/env bash
# Unit 5 TDD smoke test (codec area, Cajeta side) — written before the codec
# benchmarks. Fetches the JSON corpus, runs the harness, and asserts:
#  - json-tokenize produces 3 ok rows (twitter/citm_catalog/canada) — status ok
#    means the Cajeta tokenizer hit the EXACT expected token count per file;
#  - input_size (file bytes) is recorded so the report can derive MB/s;
#  - json-dom / json-serialize / json-roundtrip are ok on all three files;
#  - base64-encode + base64-decode are ok (round-trip cross-checks);
#  - without PROFILE_DATA_DIR, json-tokenize records a skip (no crash).
set -euo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
PROJ="$( cd -- "${SCRIPT_DIR}/.." &> /dev/null && pwd )"
REPO_ROOT="$( cd -- "${PROJ}/.." &> /dev/null && pwd )"
CAJETA="${CAJETA:-${REPO_ROOT}/build/src/cajeta}"
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
CAJETA_BENCH_SMOKE=1 PROFILE_RUN_ID=codec-run PROFILE_DATA_DIR="$DATA" "$PROJ/build/bench" --run --area codec --out "$OUT" >/tmp/codec-run.log 2>&1 \
    || { tail -20 /tmp/codec-run.log; fail "--run crashed"; }
grep -E "/(json-tokenize|base64)" /tmp/codec-run.log | sed 's/^/  /' || true

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

# json-bind-skip: the index-walk binding path runs on ALL 3 (skips numbers, no
# float gap) — bench's fastest Cajeta JSON path.
for vrt in twitter citm_catalog canada; do
    st="$(col json-bind-skip "$vrt" 28)"
    [[ "$st" == "ok" ]] || fail "json-bind-skip/$vrt status=$st"
    mn="$(col json-bind-skip "$vrt" 17)"
    echo "[codec] json-bind-skip/$vrt ok  min_ns=$mn"
done

# json DOM-family: all three corpus files run ok. The 0.7.1-era skip ("v1 DOM
# doesn't parse floats", twitter/canada) is gone (re-tested at 0.46.3,
# benchmark-reference 2.2.4).
for bench in json-dom json-serialize json-roundtrip; do
    for vrt in twitter citm_catalog canada; do
        st="$(col "$bench" "$vrt" 28)"
        [[ "$st" == "ok" ]] || fail "$bench/$vrt status=$st"
        echo "[codec] $bench/$vrt ok  min_ns=$(col "$bench" "$vrt" 17)"
    done
done

# Every row carries its catalog path and the smoke tag.
bad="$(awk -F, 'NR>1 && ($5 !~ /^codec\// || $30 !~ /smoke/)' "$OUT" | wc -l)"
[[ "$bad" == "0" ]] || fail "$bad codec rows without a codec/ area or the smoke tag"

# json-conformance: accept/reject vector all correct (self-contained)
cst="$(awk -F, 'NR>1 && $4=="json-conformance"{print $28; exit}' "$OUT")"
[[ "$cst" == "ok" ]] || fail "json-conformance status=$cst (a case misclassified)"
echo "[codec] json-conformance ok (accept/reject vector matches)"

# base64 enc/dec ok
for b in base64-encode base64-decode; do
    st="$(awk -F, -v b="$b" 'NR>1 && $4==b {print $28; exit}' "$OUT")"
    [[ "$st" == "ok" ]] || fail "$b status=$st"
    echo "[codec] $b ok"
done

# Without PROFILE_DATA_DIR, json-tokenize records a skip (no crash).
echo "[codec] no-dataset skip path..."
OUT2="/tmp/profile-codec-nodata.csv"; rm -f "$OUT2"
CAJETA_BENCH_SMOKE=1 PROFILE_RUN_ID=nd "$PROJ/build/bench" --run --area codec --out "$OUT2" >/dev/null 2>&1 || fail "--run crashed without data dir"
ST="$(awk -F, 'NR>1 && $4=="json-tokenize" {print $28; exit}' "$OUT2")"
[[ "$ST" == "skipped" ]] || fail "json-tokenize should skip without dataset, got status=$ST"

echo "CODEC OK"
