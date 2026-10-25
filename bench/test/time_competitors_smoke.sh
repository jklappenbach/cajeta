#!/usr/bin/env bash
# Phase B time smoke test — cross-language time-instant-arith (1M) /
# time-localdate-arith (100K) over the same sizes + math as the Cajeta side.
# Asserts competitors/time.sh emits schema-conformant rows for the present
# languages (rust chrono, cpp std::chrono, go time, python datetime), that every
# measured row passes its exact closed-form epoch-sum cross-check, and — guarding
# a real bug we hit — that the C++ rows aren't constant-folded away (a 1M-op loop
# must take more than a microsecond). No dataset needed.
set -uo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
PROJ="$( cd -- "${SCRIPT_DIR}/.." &> /dev/null && pwd )"
RUNNER="${PROJ}/competitors/time.sh"
NCOLS="$(awk -F, '{print NF; exit}' "${PROJ}/competitors/columns.txt")"

fail() { echo "TIME-COMP FAIL: $1" >&2; exit 1; }

echo "[time-comp] building + running time competitors (compiles cargo/g++/go on first run)…"
OUT="$(PROFILE_RUN_ID=time-comp PROFILE_RUN_TS=0 PROFILE_TRIALS=3 bash "$RUNNER" 2>/tmp/time-comp.err)" \
    || fail "time.sh exited non-zero"
[[ -n "$OUT" ]] || fail "time.sh produced no rows"

# (1) every row schema-conformant; benchmark in the expected set; area=time.
while IFS= read -r row; do
    [[ -z "$row" ]] && continue
    N="$(printf '%s' "$row" | awk -F, '{print NF}')"
    [[ "$N" == "$NCOLS" ]] || fail "row has $N cols, expected $NCOLS: $row"
    bench="$(printf '%s' "$row" | awk -F, '{print $4}')"; area="$(printf '%s' "$row" | awk -F, '{print $5}')"
    case "$bench" in time-instant-arith|time-localdate-arith) ;; *) fail "unexpected benchmark: $bench" ;; esac
    [[ "$area" == "time" ]] || fail "unexpected area: $area"
    status="$(printf '%s' "$row" | awk -F, '{print $28}')"
    if [[ "$status" == "ok" ]]; then
        min="$(printf '%s' "$row" | awk -F, '{print $17}')"
        chk="$(printf '%s' "$row" | awk -F, '{print $29}')"
        [[ "$min" =~ ^[0-9]+$ && "$min" -gt 0 ]] || fail "ok row min_ns not positive: $row"
        [[ "$chk" == "true" ]]                   || fail "ok row failed closed-form cross-check: $row"
        # anti-constant-fold guard: a 1M-op instant-arith loop can't run in <1µs.
        if [[ "$bench" == "time-instant-arith" && "$min" -lt 1000 ]]; then
            fail "instant-arith min_ns=$min looks constant-folded: $row"
        fi
    elif [[ "$status" == "skipped" ]]; then
        note="$(printf '%s' "$row" | awk -F, '{print $30}')"
        [[ -n "$note" ]] || fail "skip row missing reason: $row"
    else
        fail "unexpected status '$status': $row"
    fi
done <<< "$OUT"

# (2) rust + python produced ok rows for both benchmarks (always present).
for lang in rust python; do
    for b in time-instant-arith time-localdate-arith; do
        echo "$OUT" | awk -F, -v l="$lang" -v b="$b" '$10==l && $4==b && $28=="ok" && $29=="true"' | grep -q . \
            || fail "no ok $lang $b row"
    done
done

# (3) report.py ingests the time competitor rows.
TMP="$(mktemp -d)"
{ cat "${PROJ}/competitors/columns.txt"; echo "$OUT"; } > "$TMP/results.csv"
echo "schema_version,key,value" > "$TMP/env.csv"
python3 "${PROJ}/report/report.py" "$TMP" >/dev/null 2>&1 || fail "report.py failed on time rows"
grep -q "time-instant-arith" "$TMP/site/index.html" || fail "report site missing time rows"
rm -rf "$TMP"

NLANGS="$(echo "$OUT" | awk -F, '$28=="ok"{print $10}' | sort -u | tr '\n' ' ')"
echo "[time-comp] measured langs: [$NLANGS]; closed-form cross-checks + no constant-fold"
echo "TIME-COMP OK"
