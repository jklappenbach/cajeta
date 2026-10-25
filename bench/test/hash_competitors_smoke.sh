#!/usr/bin/env bash
# Phase B hash smoke test — cross-language bulk-throughput competitors over a
# deterministic 1 MiB buffer. Asserts competitors/hash.sh emits schema-conformant
# xxhash3/sha256/md5 rows for the present languages (rust xxhash-rust/sha2/md-5,
# cpp xxHash+OpenSSL, go zeebo+crypto, python xxhash+hashlib), that every
# measured row matches the cross-language reference digest (check_ok=true), and
# that `siphash` is recorded as a `skip` with a reason (no portable matching-key
# impl). No dataset needed — the buffer is synthetic.
set -uo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
PROJ="$( cd -- "${SCRIPT_DIR}/.." &> /dev/null && pwd )"
RUNNER="${PROJ}/competitors/hash.sh"
NCOLS="$(awk -F, '{print NF; exit}' "${PROJ}/competitors/columns.txt")"

fail() { echo "HASH-COMP FAIL: $1" >&2; exit 1; }

echo "[hash-comp] building + running hash competitors (compiles cargo/cmake/go on first run)…"
OUT="$(PROFILE_RUN_ID=hash-comp PROFILE_RUN_TS=0 PROFILE_TRIALS=3 bash "$RUNNER" 2>/tmp/hash-comp.err)" \
    || fail "hash.sh exited non-zero"
[[ -n "$OUT" ]] || fail "hash.sh produced no rows"

# (1) every row schema-conformant; benchmark in the expected set; area=hash.
while IFS= read -r row; do
    [[ -z "$row" ]] && continue
    N="$(printf '%s' "$row" | awk -F, '{print NF}')"
    [[ "$N" == "$NCOLS" ]] || fail "row has $N cols, expected $NCOLS: $row"
    bench="$(printf '%s' "$row" | awk -F, '{print $4}')"; area="$(printf '%s' "$row" | awk -F, '{print $5}')"
    case "$bench" in xxhash3|sha256|md5|siphash) ;; *) fail "unexpected benchmark: $bench" ;; esac
    [[ "$area" == "hash" ]] || fail "unexpected area: $area"
    status="$(printf '%s' "$row" | awk -F, '{print $28}')"
    if [[ "$status" == "ok" ]]; then
        min="$(printf '%s' "$row" | awk -F, '{print $17}')"
        chk="$(printf '%s' "$row" | awk -F, '{print $29}')"
        [[ "$min" =~ ^[0-9]+$ && "$min" -gt 0 ]] || fail "ok row min_ns not positive: $row"
        [[ "$chk" == "true" ]]                   || fail "ok row failed reference-digest cross-check: $row"
    elif [[ "$status" == "skipped" ]]; then
        note="$(printf '%s' "$row" | awk -F, '{print $30}')"
        [[ -n "$note" ]] || fail "skip row missing reason: $row"
    else
        fail "unexpected status '$status': $row"
    fi
done <<< "$OUT"

# (2) siphash is a skip for every language (no portable matching-key SipHash).
echo "$OUT" | awk -F, '$4=="siphash"' | grep -q . || fail "no siphash rows emitted"
echo "$OUT" | awk -F, '$4=="siphash" && $28!="skipped"' | grep -q . && fail "siphash should be skip-only"

# (3) the always-present languages (rust + python) produced measured, cross-checked rows
#     for all three real algorithms.
for lang in rust python; do
    for b in xxhash3 sha256 md5; do
        echo "$OUT" | awk -F, -v l="$lang" -v b="$b" '$10==l && $4==b && $28=="ok" && $29=="true"' | grep -q . \
            || fail "no ok $lang $b row"
    done
done

# (4) report.py ingests the hash competitor rows.
TMP="$(mktemp -d)"
{ cat "${PROJ}/competitors/columns.txt"; echo "$OUT"; } > "$TMP/results.csv"
echo "schema_version,key,value" > "$TMP/env.csv"
python3 "${PROJ}/report/report.py" "$TMP" >/dev/null 2>&1 || fail "report.py failed on hash rows"
grep -q "xxhash3" "$TMP/site/index.html" || fail "report site missing xxhash3 rows"
rm -rf "$TMP"

NLANGS="$(echo "$OUT" | awk -F, '$28=="ok"{print $10}' | sort -u | tr '\n' ' ')"
echo "[hash-comp] measured langs: [$NLANGS]; siphash skipped with reason"
echo "HASH-COMP OK"
