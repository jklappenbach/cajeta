#!/usr/bin/env bash
# bench/lang/ smoke, competitor side (benchmark-reference plan 5.1.2).
#
# Runs the eight unit-5 lang programs (drivers/lang/{loops,exceptions,alloc,
# dispatch,text,values,mathscalar,guid}.sh) through competitors/run.sh in
# short mode and asserts, for each of cpp, rust, go, java and python, that
# every id unit 5 added (test/lang_lib.sh LANG_IDS_UNIT5, with its variants)
# has a schema-v2 row, area lang, platform = language, and that the row is
# either ok with check_ok true or skipped with a reason. The skips are the
# ones the pages call for, and only those (EXPECTED_SKIPS below). Java's
# exception rows must come with and without a stack trace; Rust's and Go's
# exception rows must say they propagate a Result / return an error.
#
# Python runs its 1e8 loops at 1/100 (PROFILE_PY_DIVISOR=100; the rows carry
# `scaled 1/100`): a full-size CPython pass is about 75 minutes.
# Env: CAJETA_BENCH_OUT (default /tmp/cajeta-bench-lang-smoke).
set -uo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
source "$SCRIPT_DIR/csv_lib.sh"   # cawk / csv_fields: CSV-aware column checks
PROJ="$( cd -- "${SCRIPT_DIR}/.." &> /dev/null && pwd )"
OUTD="${CAJETA_BENCH_OUT:-/tmp/cajeta-bench-lang-smoke}"
NCOLS="$(cawk '{print NF; exit}' "${PROJ}/competitors/columns.txt")"
# shellcheck source=lang_lib.sh
source "$SCRIPT_DIR/lang_lib.sh"
fail() { echo "LANG-COMP FAIL: $1" >&2; exit 1; }
mkdir -p "$OUTD"
CSV="$OUTD/lang-competitors.csv"

# language:slug[:variant] that must be skipped (no counterpart in that language).
EXPECTED_SKIPS="
java:loop-counted-nobounds:bounds-off python:loop-counted-nobounds:bounds-off
cpp:throw-recoverable rust:throw-recoverable go:throw-recoverable python:throw-recoverable
cpp:throw-in-fiber rust:throw-in-fiber
cpp:alloc-live-set-on rust:alloc-live-set-on go:alloc-live-set-on java:alloc-live-set-on python:alloc-live-set-on
cpp:alloc-live-set-off rust:alloc-live-set-off go:alloc-live-set-off java:alloc-live-set-off python:alloc-live-set-off
"

t0=$SECONDS
PROFILE_RUN_ID=lang-comp PROFILE_RUN_TS=0 PROFILE_WARMUP=1 PROFILE_TRIALS=3 PROFILE_PY_DIVISOR=100 \
    bash "$PROJ/competitors/run.sh" lang loops exceptions alloc dispatch text values mathscalar guid \
    > "$CSV" 2> "$OUTD/lang-competitors.err" || { tail -5 "$OUTD/lang-competitors.err"; fail "run.sh lang exited non-zero"; }
echo "[lang-comp] ran the eight programs in $((SECONDS - t0))s"
[[ -s "$CSV" ]] || fail "no rows"

while IFS= read -r row; do
    [[ -z "$row" ]] && continue
    N="$(printf '%s' "$row" | cawk '{print NF}')"
    [[ "$N" == "$NCOLS" ]] || fail "row has $N columns, want $NCOLS: $row"
    printf '%s' "$row" | cawk '$1!="2" || $5!="lang" || $33=="" || $34=="" || $37!=$10 {exit 1}' \
        || fail "row is not v2 lang with machine/os/platform: $row"
done < <(csv_fields "$CSV")

ok=0; sk=0
for lang in cpp rust go java python; do
    for e in $LANG_IDS_UNIT5; do
        slug="${e%%:*}"; var=""; [[ "$e" == *:* ]] && var="${e#*:}"
        # The competitor variants of throw-recoverable are Java's; others skip it once, unlabelled.
        if [[ "$slug" == "throw-recoverable" && "$lang" != "java" ]]; then
            [[ "$var" == "recoverable" ]] || continue
            var=""
        fi
        rows="$(cawk -v l="$lang" -v b="$slug" -v v="$var" '$10==l && $4==b && $31==v' "$CSV")"
        [[ -n "$rows" ]] || fail "no $lang row for lang/$slug${var:+ [$var]}"
        key="$lang:$slug${var:+:$var}"
        if [[ " $(echo $EXPECTED_SKIPS) " == *" $key "* ]]; then
            echo "$rows" | cawk '$28!="skipped" || $30=="" {exit 1}' || fail "$key should be skipped with a reason: $rows"
            sk=$((sk + 1))
        else
            echo "$rows" | cawk '$28!="ok" || $29!="true" {exit 1}' || fail "$key not ok/true: $rows"
            ok=$((ok + $(echo "$rows" | wc -l)))
        fi
    done
done

# Exceptions notes and Java's two libraries.
for b in try-no-throw throw-catch-1 throw-catch-10 throw-catch-100; do
    cawk -v b="$b" '$10=="rust" && $4==b && $30 ~ /Result/' "$CSV" | grep -q . || fail "rust $b notes do not say Result"
    cawk -v b="$b" '$10=="go" && $4==b && $30 ~ /error/' "$CSV" | grep -q . || fail "go $b notes do not say error"
done
for b in throw-catch-1 throw-catch-10 throw-catch-100 throw-in-fiber; do
    n="$(cawk -v b="$b" '$10=="java" && $4==b && $28=="ok" {print $12}' "$CSV" | sort -u | wc -l)"
    [[ "$n" -ge 2 ]] || fail "java $b lacks the with/without stack trace rows"
done
# Java's template row is the erased generic, boxed.
cawk '$10=="java" && $4=="call-template" && $30 ~ /box/' "$CSV" | grep -q . || fail "java call-template notes do not say boxed"

echo "LANG-COMP OK ($ok ok rows, $sk skipped as the pages call for, $((SECONDS - t0))s)"
