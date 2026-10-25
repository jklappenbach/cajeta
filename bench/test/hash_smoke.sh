#!/usr/bin/env bash
# Unit 9 TDD smoke test (hashing & crypto, Cajeta side). Self-generating. Asserts
# each hash benchmark is ok (non-zero digest / correct digest size) with input_size.
set -euo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
PROJ="$( cd -- "${SCRIPT_DIR}/.." &> /dev/null && pwd )"
REPO_ROOT="$( cd -- "${PROJ}/.." &> /dev/null && pwd )"
CAJETA="${CAJETA:-${REPO_ROOT}/build/src/cajeta}"

fail() { echo "HASH FAIL: $1" >&2; exit 1; }
[[ -x "$CAJETA" ]] || fail "cajeta not found at $CAJETA"

cd "$PROJ"
echo "[hash] building (release)..."
"$CAJETA" release >/dev/null || fail "release build failed"

OUT="/tmp/profile-hash.csv"
rm -f "$OUT"
CAJETA_BENCH_SMOKE=1 PROFILE_RUN_ID=hash-run "$PROJ/build/bench" --run --area hash --out "$OUT" >/tmp/hash-run.log 2>&1 \
    || { grep -vE "example:" /tmp/hash-run.log | tail -15; fail "--run crashed"; }
grep -E "measured (xxhash|siphash)" /tmp/hash-run.log | sed 's/^/  /'

for b in xxhash3 xxhash3-256k xxhash3_128 xxhash3_128-256k siphash; do
    row="$(awk -F, -v b="$b" 'NR>1 && $4==b {print; exit}' "$OUT")"
    [[ -n "$row" ]] || fail "no row for $b"
    st="$(printf '%s' "$row" | awk -F, '{print $28}')"
    ar="$(printf '%s' "$row" | awk -F, '{print $5}')"
    [[ "$ar" == hash/* ]] || fail "$b area=$ar (want the hash/ catalog path)"
    printf '%s' "$row" | awk -F, '{print $30}' | grep -q '^smoke' || fail "$b row not tagged smoke"
    [[ "$st" == "ok" ]] || fail "$b status=$st"
    mb="$(printf '%s' "$row" | awk -F, '{print $9, $17}')"
    echo "[hash] $b ok  (bytes,min_ns)=$mb"
done

echo "HASH OK"
