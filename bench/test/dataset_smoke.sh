#!/usr/bin/env bash
# Unit 3 TDD smoke test (dataset + env slice) — written before fetch.sh/env-capture.sh.
# Asserts: fetch-on-demand downloads + SHA-256-verifies; a cached valid file is
# NOT refetched; a tampered file is detected stale + restored; a wrong expected
# checksum fails loudly; and env-capture emits the host block.
# Requires network (skips with a clear message if unreachable).
set -euo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
PROJ="$( cd -- "${SCRIPT_DIR}/.." &> /dev/null && pwd )"
FETCH="${PROJ}/datasets/fetch.sh"
ENVCAP="${PROJ}/scripts/env-capture.sh"
CACHE="/tmp/profile-ds-test-cache"
export PROFILE_DATA_DIR="$CACHE"
TW_SHA="a08b769f32b95f426cbc3abafcec65c1a19d3eb544d4ddf320eae142c99efc5d"

fail() { echo "DATASET FAIL: $1" >&2; exit 1; }

# Network precheck — skip (not fail) when offline.
if ! timeout 12 curl -fsSL --max-time 10 -o /dev/null \
       "https://raw.githubusercontent.com/miloyip/nativejson-benchmark/master/data/twitter.json" 2>/dev/null; then
    echo "DATASET SKIP: network unreachable (fetch path untestable here)"
    exit 0
fi

rm -rf "$CACHE"

# (1) fresh fetch downloads + verifies all three
echo "[dataset] fresh fetch..."
OUT1="$(bash "$FETCH" 2>&1)" || { printf '%s\n' "$OUT1"; fail "fetch failed"; }
for n in twitter citm_catalog canada; do
    [[ -f "$CACHE/$n.json" ]] || fail "missing $n.json after fetch"
done
printf '%s\n' "$OUT1" | grep -q "\[ok\].*twitter" || fail "twitter not verified on first fetch"

# (2) cached: second run reuses, does not refetch
echo "[dataset] cached run..."
OUT2="$(bash "$FETCH" 2>&1)"
printf '%s\n' "$OUT2" | grep -q "\[cached\].*twitter" || fail "twitter not served from cache"
if printf '%s\n' "$OUT2" | grep -q "\[fetch\]"; then fail "unexpected refetch on cached run"; fi

# (3) tamper → detected stale → restored
echo "[dataset] tamper + restore..."
echo "garbage" >> "$CACHE/twitter.json"
OUT3="$(bash "$FETCH" twitter 2>&1)" || fail "refetch after tamper failed"
printf '%s\n' "$OUT3" | grep -q "\[stale\].*twitter" || fail "tamper not detected"
GOT="$(sha256sum "$CACHE/twitter.json" | cut -d' ' -f1)"
[[ "$GOT" == "$TW_SHA" ]] || fail "twitter sha not restored after tamper"

# (4) wrong expected checksum fails loudly
echo "[dataset] checksum-mismatch must fail..."
TMPMAN="/tmp/profile-bad-manifest.tsv"
printf 'badone\t%s\t10\tdeadbeef\n' \
    "https://raw.githubusercontent.com/miloyip/nativejson-benchmark/master/data/twitter.json" > "$TMPMAN"
if PROFILE_MANIFEST="$TMPMAN" bash "$FETCH" badone >/dev/null 2>&1; then
    fail "expected loud failure on checksum mismatch, got exit 0"
fi

# (5) env-capture emits the host block
echo "[dataset] env-capture..."
ENVOUT="$(bash "$ENVCAP")"
for key in cpu_model cpu_cores kernel governor mem_total_kb; do
    printf '%s\n' "$ENVOUT" | grep -q "^${key}," || fail "env-capture missing $key"
done

echo "DATASET OK"
