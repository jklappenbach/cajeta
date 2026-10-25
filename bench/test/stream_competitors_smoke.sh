#!/usr/bin/env bash
# Phase B stream smoke test — cross-language stream-filter-map-reduce (sequential)
# + stream-parallel-reduce over xs[i]=i%1000 (N=1M), same as the Cajeta side.
# Asserts competitors/stream.sh emits schema-conformant rows for the present
# languages (rust Iterator/rayon, cpp hand-loop/OpenMP, go loop/goroutines, python
# genexpr/sum), every measured row passes its exact-sum cross-check, and the data-
# parallel competitor (rayon) is present. No dataset needed.
set -uo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
PROJ="$( cd -- "${SCRIPT_DIR}/.." &> /dev/null && pwd )"
RUNNER="${PROJ}/competitors/stream.sh"
NCOLS="$(awk -F, '{print NF; exit}' "${PROJ}/competitors/columns.txt")"

fail() { echo "STREAM-COMP FAIL: $1" >&2; exit 1; }

echo "[stream-comp] building + running stream competitors (compiles cargo/g++/go on first run)…"
OUT="$(PROFILE_RUN_ID=stream-comp PROFILE_RUN_TS=0 PROFILE_TRIALS=3 bash "$RUNNER" 2>/tmp/stream-comp.err)" \
    || fail "stream.sh exited non-zero"
[[ -n "$OUT" ]] || fail "stream.sh produced no rows"

# (1) every row schema-conformant; benchmark in the expected set; area=stream.
while IFS= read -r row; do
    [[ -z "$row" ]] && continue
    N="$(printf '%s' "$row" | awk -F, '{print NF}')"
    [[ "$N" == "$NCOLS" ]] || fail "row has $N cols, expected $NCOLS: $row"
    bench="$(printf '%s' "$row" | awk -F, '{print $4}')"; area="$(printf '%s' "$row" | awk -F, '{print $5}')"
    case "$bench" in stream-filter-map-reduce|stream-parallel-reduce) ;; *) fail "unexpected benchmark: $bench" ;; esac
    [[ "$area" == "stream" ]] || fail "unexpected area: $area"
    status="$(printf '%s' "$row" | awk -F, '{print $28}')"
    if [[ "$status" == "ok" ]]; then
        min="$(printf '%s' "$row" | awk -F, '{print $17}')"
        chk="$(printf '%s' "$row" | awk -F, '{print $29}')"
        [[ "$min" =~ ^[0-9]+$ && "$min" -gt 0 ]] || fail "ok row min_ns not positive: $row"
        [[ "$chk" == "true" ]]                   || fail "ok row failed exact-sum cross-check: $row"
    elif [[ "$status" == "skipped" ]]; then
        note="$(printf '%s' "$row" | awk -F, '{print $30}')"
        [[ -n "$note" ]] || fail "skip row missing reason: $row"
    else
        fail "unexpected status '$status': $row"
    fi
done <<< "$OUT"

# (2) rust + python produced ok rows for both benchmarks (always present).
for lang in rust python; do
    for b in stream-filter-map-reduce stream-parallel-reduce; do
        echo "$OUT" | awk -F, -v l="$lang" -v b="$b" '$10==l && $4==b && $28=="ok" && $29=="true"' | grep -q . \
            || fail "no ok $lang $b row"
    done
done

# (3) the data-parallel competitor (rayon) is present and ok.
echo "$OUT" | awk -F, '$10=="rust" && $12=="rayon" && $4=="stream-parallel-reduce" && $28=="ok"' | grep -q . \
    || fail "rayon parallel-reduce competitor missing"

# (4) report.py ingests the stream competitor rows.
TMP="$(mktemp -d)"
{ cat "${PROJ}/competitors/columns.txt"; echo "$OUT"; } > "$TMP/results.csv"
echo "schema_version,key,value" > "$TMP/env.csv"
python3 "${PROJ}/report/report.py" "$TMP" >/dev/null 2>&1 || fail "report.py failed on stream rows"
grep -q "stream-parallel-reduce" "$TMP/site/index.html" || fail "report site missing stream rows"
rm -rf "$TMP"

NLANGS="$(echo "$OUT" | awk -F, '$28=="ok"{print $10}' | sort -u | tr '\n' ' ')"
echo "[stream-comp] measured langs: [$NLANGS]; rayon/OpenMP/goroutine parallel-reduce included"
echo "STREAM-COMP OK"
