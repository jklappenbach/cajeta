#!/usr/bin/env bash
# Phase B codec smoke test — the cross-language JSON parse-to-DOM competitors.
# Asserts competitors/codec.sh emits schema-conformant `json-dom` rows for the
# present languages (serde_json/simd-json, simdjson/yyjson, encoding-json/goccy,
# json/orjson/msgspec/ujson), that every measured row passes its element-count
# cross-check (check_ok=true), and that a missing toolchain/library is recorded
# as a `skip` row WITH a reason rather than crashing. Requires the corpus; skips
# cleanly if it isn't fetched.
set -uo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
PROJ="$( cd -- "${SCRIPT_DIR}/.." &> /dev/null && pwd )"
RUNNER="${PROJ}/competitors/codec.sh"
DATA="${PROFILE_DATA_DIR:-${PROJ}/datasets/cache}"
NCOLS="$(awk -F, '{print NF; exit}' "${PROJ}/competitors/columns.txt")"

fail() { echo "CODEC-COMP FAIL: $1" >&2; exit 1; }

# Corpus required — the competitors parse it. Skip the test if unfetched.
if [[ ! -f "$DATA/twitter.json" ]]; then
    echo "[codec-comp] corpus absent ($DATA) — run datasets/fetch.sh; skipping"
    echo "CODEC-COMP OK (skipped: no corpus)"
    exit 0
fi

echo "[codec-comp] building + running codec competitors (compiles cargo/cmake/go on first run)…"
OUT="$(PROFILE_RUN_ID=codec-comp PROFILE_RUN_TS=0 PROFILE_TRIALS=3 PROFILE_DATA_DIR="$DATA" bash "$RUNNER" 2>/tmp/codec-comp.err)" \
    || fail "codec.sh exited non-zero"
[[ -n "$OUT" ]] || fail "codec.sh produced no rows"

# (1) every row is schema-conformant (column count) and is the json-dom/codec benchmark.
while IFS= read -r row; do
    [[ -z "$row" ]] && continue
    N="$(printf '%s' "$row" | awk -F, '{print NF}')"
    [[ "$N" == "$NCOLS" ]] || fail "row has $N cols, expected $NCOLS: $row"
    bench="$(printf '%s' "$row" | awk -F, '{print $4}')"; area="$(printf '%s' "$row" | awk -F, '{print $5}')"
    [[ "$bench" == "json-dom" ]] || fail "unexpected benchmark: $bench"
    [[ "$area" == "codec" ]]     || fail "unexpected area: $area"
    status="$(printf '%s' "$row" | awk -F, '{print $28}')"
    if [[ "$status" == "ok" ]]; then
        # measured rows: positive timing + throughput + cross-check passed
        min="$(printf '%s' "$row" | awk -F, '{print $17}')"
        thr="$(printf '%s' "$row" | awk -F, '{print $21}')"
        chk="$(printf '%s' "$row" | awk -F, '{print $29}')"
        [[ "$min" =~ ^[0-9]+$ && "$min" -gt 0 ]] || fail "ok row min_ns not positive: $row"
        [[ "$thr" =~ ^[0-9.]+$ ]]                || fail "ok row throughput not numeric: $row"
        [[ "$chk" == "true" ]]                   || fail "ok row failed element-count cross-check: $row"
    elif [[ "$status" == "skip" ]]; then
        # skip rows must carry a human reason in notes (col 30)
        note="$(printf '%s' "$row" | awk -F, '{print $30}')"
        [[ -n "$note" ]] || fail "skip row missing reason: $row"
    else
        fail "unexpected status '$status': $row"
    fi
done <<< "$OUT"

# (2) the always-present languages (rust + python) produced measured, cross-checked rows.
echo "$OUT" | awk -F, '$10=="rust"   && $28=="ok" && $29=="true"' | grep -q . || fail "no ok rust rows"
echo "$OUT" | awk -F, '$10=="python" && $28=="ok" && $29=="true"' | grep -q . || fail "no ok python rows"
# serde_json (rust) and json stdlib (python) are guaranteed libraries.
echo "$OUT" | grep -q ",serde_json," || fail "serde_json row missing"
echo "$OUT" | awk -F, '$10=="python" && $12=="json" && $28=="ok"' | grep -q . || fail "python stdlib json row missing"

# (3) report.py ingests the competitor rows alongside Cajeta rows (codec area).
TMP="$(mktemp -d)"
{ cat "${PROJ}/competitors/columns.txt"; echo "$OUT"; } > "$TMP/results.csv"
echo "schema_version,key,value" > "$TMP/env.csv"   # minimal env block
python3 "${PROJ}/report/report.py" "$TMP" >/dev/null 2>&1 || fail "report.py failed on competitor rows"
grep -q "json-dom" "$TMP/site/index.html" || fail "report site missing json-dom rows"
grep -q "simdjson\|serde_json\|orjson" "$TMP/site/index.html" || fail "report site missing competitor libraries"
rm -rf "$TMP"

NLANGS="$(echo "$OUT" | awk -F, '$28=="ok"{print $10}' | sort -u | tr '\n' ' ')"
NLIBS="$(echo "$OUT" | awk -F, '$28=="ok"{print $12}' | sort -u | wc -l)"
echo "[codec-comp] measured langs: [$NLANGS] across $NLIBS libraries; skips carry reasons"
echo "CODEC-COMP OK"
