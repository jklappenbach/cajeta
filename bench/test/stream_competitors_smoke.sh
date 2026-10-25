#!/usr/bin/env bash
# Phase B stream smoke test — cross-language stream-filter-map-reduce (sequential)
# + stream-parallel-reduce over xs[i]=i%1000 (N=1M), same as the Cajeta side.
# Asserts competitors/lang.sh stream emits schema-conformant rows for the present
# languages (rust Iterator/rayon, cpp hand-loop/OpenMP, go loop/goroutines, python
# genexpr/sum), every measured row passes its exact-sum cross-check, and the data-
# parallel competitor (rayon) is present. No dataset needed.
set -uo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
source "$SCRIPT_DIR/csv_lib.sh"   # cawk / csv_fields: CSV-aware column checks
PROJ="$( cd -- "${SCRIPT_DIR}/.." &> /dev/null && pwd )"
RUNNER="${PROJ}/competitors/lang.sh"   # program stream of package lang
NCOLS="$(cawk '{print NF; exit}' "${PROJ}/competitors/columns.txt")"

fail() { echo "STREAM-COMP FAIL: $1" >&2; exit 1; }

echo "[stream-comp] building + running stream competitors (compiles cargo/g++/go on first run)…"
OUT="$(PROFILE_RUN_ID=stream-comp PROFILE_RUN_TS=0 PROFILE_TRIALS=3 bash "$RUNNER" stream 2>/tmp/stream-comp.err)" \
    || fail "stream.sh exited non-zero"
[[ -n "$OUT" ]] || fail "stream.sh produced no rows"

# (1) every row schema-conformant; benchmark in the expected set; area=stream.
while IFS= read -r row; do
    [[ -z "$row" ]] && continue
    N="$(printf '%s' "$row" | cawk '{print NF}')"
    [[ "$N" == "$NCOLS" ]] || fail "row has $N cols, expected $NCOLS: $row"
    [[ "$(printf '%s' "$row" | cawk '{print $1}')" == "2" ]] || fail "row is not schema_version 2: $row"
    printf '%s' "$row" | cawk '$33=="" || $34=="" || $37=="" || $37!=$10 {exit 1}' \
        || fail "row lacks machine/os/platform (platform must be the language): $row"
    bench="$(printf '%s' "$row" | cawk '{print $4}')"; area="$(printf '%s' "$row" | cawk '{print $5}')"
    case "$bench" in stream-filter-map-reduce|stream-parallel-reduce) ;; *) fail "unexpected benchmark: $bench" ;; esac
    [[ "$area" == "lang" ]] || fail "unexpected area: $area"
    status="$(printf '%s' "$row" | cawk '{print $28}')"
    if [[ "$status" == "ok" ]]; then
        min="$(printf '%s' "$row" | cawk '{print $17}')"
        chk="$(printf '%s' "$row" | cawk '{print $29}')"
        [[ "$min" =~ ^[0-9]+$ && "$min" -gt 0 ]] || fail "ok row min_ns not positive: $row"
        [[ "$chk" == "true" ]]                   || fail "ok row failed exact-sum cross-check: $row"
    elif [[ "$status" == "skipped" ]]; then
        note="$(printf '%s' "$row" | cawk '{print $30}')"
        [[ -n "$note" ]] || fail "skip row missing reason: $row"
    else
        fail "unexpected status '$status': $row"
    fi
done < <(csv_fields <<<"$OUT")

# (2) rust + python produced ok rows for both benchmarks (always present).
for lang in rust python; do
    for b in stream-filter-map-reduce stream-parallel-reduce; do
        echo "$OUT" | cawk -v l="$lang" -v b="$b" '$10==l && $4==b && $28=="ok" && $29=="true"' | grep -q . \
            || fail "no ok $lang $b row"
    done
done

# (3) the data-parallel competitor (rayon) is present and ok.
echo "$OUT" | cawk '$10=="rust" && $12=="rayon" && $4=="stream-parallel-reduce" && $28=="ok"' | grep -q . \
    || fail "rayon parallel-reduce competitor missing"


NLANGS="$(echo "$OUT" | cawk '$28=="ok"{print $10}' | sort -u | tr '\n' ' ')"
echo "[stream-comp] measured langs: [$NLANGS]; rayon/OpenMP/goroutine parallel-reduce included"
echo "STREAM-COMP OK"
