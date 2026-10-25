#!/usr/bin/env bash
# codec/ competitor smoke (benchmark-reference unit 9). Runs every codec
# program (competitors/codec.sh: drivers/codec/{json,basen,csv}.sh) in short
# mode on the smoke inputs and asserts:
#  - every row is schema v2 (37 columns), area codec, platform = language;
#  - every row is ok with check_ok true, or skipped with a reason; nothing is
#    invalid, and an ok row has a positive median (nothing elided);
#  - each library the pages name emits each of its ids ok:
#      json   simdjson, serde_json, encoding/json, Jackson: all 16 rows
#             (5 ids x 3 corpus files + conformance); yyjson, json, orjson:
#             dom/serialize/roundtrip/conformance (no tokenizer or binder:
#             those six rows are skips with the reason)
#      base64 simdutf, Rust base64, encoding/base64, java.util.Base64, Python base64
#      base32 data-encoding, encoding/base32, commons-codec, Python base64
#      csv    Rust csv, encoding/csv, univocity, pandas, the csv module (parse
#             and write); fast-cpp-csv-parser parse (it has no writer: a skip)
# The Deflate competitors live in cajeta-codec/bench (its bench_smoke.sh).
# Env: PROFILE_DATA_DIR (default /tmp/profile-codec-data, filled by
# datasets/fetch.sh + gen_csv.py smoke when absent), CAJETA_BENCH_OUT (where
# the rows and stderr land; default /tmp).
set -uo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
PROJ="$( cd -- "${SCRIPT_DIR}/.." &> /dev/null && pwd )"
DATA="${PROFILE_DATA_DIR:-/tmp/profile-codec-data}"
OUTDIR="${CAJETA_BENCH_OUT:-/tmp}"
NCOLS="$(awk -F, '{print NF; exit}' "${PROJ}/competitors/columns.txt")"
mkdir -p "$OUTDIR"

fail() { echo "CODEC-COMP FAIL: $1" >&2; exit 1; }

if [[ ! -f "$DATA/twitter.json" || ! -f "$DATA/jsonchecker_fail33.json" ]]; then
    PROFILE_DATA_DIR="$DATA" bash "$PROJ/datasets/fetch.sh" >/dev/null 2>&1 \
        || { echo "[codec-comp] corpus not fetchable (offline?) — skipping"; echo "CODEC-COMP OK (skipped: no corpus)"; exit 0; }
fi
python3 "$PROJ/datasets/gen_csv.py" "$DATA" smoke >/dev/null || fail "gen_csv.py smoke failed"

OUT="$OUTDIR/codec-comp.csv"; ERR="$OUTDIR/codec-comp.err"
echo "[codec-comp] building + running json, basen, csv in every language (short mode)..."
t0=$SECONDS
PROFILE_RUN_ID=codec-comp PROFILE_RUN_TS=0 PROFILE_TRIALS=3 CAJETA_BENCH_SMOKE=1 PROFILE_DATA_DIR="$DATA" \
    bash "$PROJ/competitors/codec.sh" >"$OUT" 2>"$ERR" || fail "codec.sh exited non-zero (see $ERR)"
echo "[codec-comp] ran in $((SECONDS - t0)) s, $(wc -l < "$OUT") rows"
[[ -s "$OUT" ]] || fail "codec.sh produced no rows"
grep -q 'rows_v2: dropped' "$ERR" && fail "rows_v2 dropped a malformed line (see $ERR)"

# (1) shape, status, reasons, no elision.
awk -F, -v n="$NCOLS" '
    NF != n                         { print "cols " NF ": " $0; bad = 1; next }
    $1 != "2" || $5 != "codec"      { print "schema/area: " $0; bad = 1 }
    $33 == "" || $34 == "" || $37 != $10 { print "machine/os/platform: " $0; bad = 1 }
    $28 == "ok" && ($29 != "true" || $18 !~ /^[0-9]+$/ || $18 + 0 <= 0) { print "ok row: " $0; bad = 1 }
    $28 == "skipped" && $30 == ""   { print "skip without reason: " $0; bad = 1 }
    $28 != "ok" && $28 != "skipped" { print "status " $28 ": " $0; bad = 1 }
    END { exit bad }' "$OUT" || fail "malformed or failing rows above"

# (2) the libraries the pages name, each with its ids ok.
JSON16=""
for b in json-tokenize json-bind-skip json-dom json-serialize json-roundtrip; do
    for v in twitter citm_catalog canada; do JSON16+=" $b:$v"; done
done
JSON16+=" json-conformance:"
JSON10=""
for b in json-dom json-serialize json-roundtrip; do
    for v in twitter citm_catalog canada; do JSON10+=" $b:$v"; done
done
JSON10+=" json-conformance:"

has_ok() {   # lang library-regex id:variant...
    local lang="$1" lib="$2" e b v; shift 2
    for e in "$@"; do
        b="${e%%:*}"; v="${e#*:}"
        awk -F, -v l="$lang" -v lib="$lib" -v b="$b" -v v="$v" \
            '$10==l && $12 ~ ("^(" lib ")$") && $4==b && $31==v && $28=="ok" && $29=="true" {f=1} END {exit !f}' "$OUT" \
            || fail "$lang $lib: no ok row for $b/${v:-.}"
    done
    echo "[codec-comp] $lang $lib: $# ok"
}
has_skip() {   # lang library id...
    local lang="$1" lib="$2" b; shift 2
    for b in "$@"; do
        awk -F, -v l="$lang" -v lib="$lib" -v b="$b" '$10==l && $12==lib && $4==b && $28=="skipped" {f=1} END {exit !f}' "$OUT" \
            || fail "$lang $lib: $b should be a recorded skip"
    done
}

has_ok cpp simdjson $JSON16
has_ok cpp yyjson $JSON10;          has_skip cpp yyjson json-tokenize json-bind-skip
has_ok rust serde_json $JSON16
has_ok go encoding/json $JSON16
has_ok java 'jackson-core|jackson-databind' $JSON16
has_ok python json $JSON10;         has_skip python json json-tokenize json-bind-skip
has_ok python orjson $JSON10;       has_skip python orjson json-tokenize json-bind-skip

has_ok cpp simdutf base64-encode: base64-decode:
has_ok rust base64 base64-encode: base64-decode:
has_ok go encoding/base64 base64-encode: base64-decode:
has_ok java java.util.Base64 base64-encode: base64-decode:
has_ok python base64 base64-encode: base64-decode: base32-encode: base32-decode:
has_ok rust data-encoding base32-encode: base32-decode:
has_ok go encoding/base32 base32-encode: base32-decode:
has_ok java commons-codec base32-encode: base32-decode:

has_ok rust csv csv-parse: csv-write:
has_ok go encoding/csv csv-parse: csv-write:
has_ok java univocity-parsers csv-parse: csv-write:
has_ok python pandas csv-parse: csv-write:
has_ok python csv csv-parse: csv-write:
has_ok cpp fast-cpp-csv-parser csv-parse:; has_skip cpp fast-cpp-csv-parser csv-write

# (3) csv rows ran on the smoke file and say so.
awk -F, '$4 ~ /^csv-/ && $28 == "ok" && ($6 != "codec-csv-smoke" || $30 !~ /smoke input 4 MB/) {exit 1}' "$OUT" \
    || fail "a csv row did not record the smoke input"

echo "[codec-comp] $(awk -F, '$28=="ok"' "$OUT" | wc -l) ok rows, $(awk -F, '$28=="skipped"' "$OUT" | wc -l) skips with reasons"
echo "CODEC-COMP OK"
