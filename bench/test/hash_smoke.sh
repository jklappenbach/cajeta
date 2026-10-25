#!/usr/bin/env bash
# Unit 9 TDD smoke test (hashing & crypto, Cajeta side). Self-generating. Asserts
# each hash benchmark is ok (non-zero digest / correct digest size) with input_size.
set -euo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
PROJ="$( cd -- "${SCRIPT_DIR}/.." &> /dev/null && pwd )"
REPO_ROOT="$( cd -- "${PROJ}/../.." &> /dev/null && pwd )"
CAJETA="${CAJETA:-${REPO_ROOT}/build-cajeta/src/cajeta}"

fail() { echo "HASH FAIL: $1" >&2; exit 1; }
[[ -x "$CAJETA" ]] || fail "cajeta not found at $CAJETA"

cd "$PROJ"
echo "[hash] building (release)..."
"$CAJETA" release >/dev/null || fail "release build failed"

OUT="/tmp/profile-hash.csv"
rm -f "$OUT"
PROFILE_RUN_ID=hash-run "$PROJ/build/profile" --run --out "$OUT" >/tmp/hash-run.log 2>&1 \
    || { grep -vE "example:" /tmp/hash-run.log | tail -15; fail "--run crashed"; }
grep -E "measured (xxhash|siphash|sha256|md5)" /tmp/hash-run.log | sed 's/^/  /'

for b in xxhash3 siphash sha256 md5; do
    row="$(awk -F, -v b="$b" 'NR>1 && $4==b {print; exit}' "$OUT")"
    [[ -n "$row" ]] || fail "no row for $b"
    st="$(printf '%s' "$row" | awk -F, '{print $28}')"
    [[ "$st" == "ok" ]] || fail "$b status=$st"
    mb="$(printf '%s' "$row" | awk -F, '{print $9, $17}')"
    echo "[hash] $b ok  (bytes,min_ns)=$mb"
done

echo "HASH OK"
