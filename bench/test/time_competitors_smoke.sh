#!/usr/bin/env bash
# time/ competitor smoke (benchmark-reference unit 8, spec §6.4). Runs every
# competitors/drivers/time/*.sh program (`time`: time-instant-arith and
# time-localdate-arith; `datetime`: the other seven ids) and asserts:
#   (1) every row is schema v2 with the catalog id, machine, os and platform;
#   (2) each id on bench/time/*.md has an ok, check_ok=true row from every
#       competitor library its page names (table below);
#   (3) anti-elision: an ok row of N operations took at least N/10 ns (a
#       folded loop runs in microseconds);
#   (4) any skipped row carries a reason.
# Env: PROFILE_TRIALS (3 here), PROFILE_WARMUP (1 here), CAJETA_BENCH_OUT
# (where the rows and stderr land; default /tmp).
set -uo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
source "$SCRIPT_DIR/csv_lib.sh"   # cawk / csv_fields: CSV-aware column checks
PROJ="$( cd -- "${SCRIPT_DIR}/.." &> /dev/null && pwd )"
RUNNER="${PROJ}/competitors/time.sh"
NCOLS="$(cawk '{print NF; exit}' "${PROJ}/competitors/columns.txt")"
OUTDIR="${CAJETA_BENCH_OUT:-/tmp}"; mkdir -p "$OUTDIR"

fail() { echo "TIME-COMP FAIL: $1" >&2; exit 1; }

echo "[time-comp] building + running time competitors (cargo, g++, go, javac on first run)..."
t0=$SECONDS
OUT="$(PROFILE_RUN_ID=time-comp PROFILE_RUN_TS=0 PROFILE_TRIALS="${PROFILE_TRIALS:-3}" PROFILE_WARMUP="${PROFILE_WARMUP:-1}" \
        bash "$RUNNER" 2>"$OUTDIR/time-comp.err")" || fail "time.sh exited non-zero (see $OUTDIR/time-comp.err)"
printf '%s\n' "$OUT" > "$OUTDIR/time-comp.csv"
echo "[time-comp] ran in $((SECONDS - t0))s"
[[ -n "$OUT" ]] || fail "time.sh produced no rows"

# id ops lang/library ... (the page's competitors; `|` separates alternatives)
REQUIRED='
time-instant-arith 10000000 cpp/std::chrono rust/jiff rust/chrono go/time java/java.time python/datetime python/whenever
time-localdate-arith 10000000 cpp/std::chrono rust/jiff rust/chrono go/time java/java.time python/datetime python/whenever
clock-now 10000000 cpp/std::chrono rust/std go/time java/java.time python/time
duration-arith 10000000 cpp/std::chrono rust/jiff rust/chrono go/time java/java.time python/datetime python/whenever
localtime-arith 10000000 cpp/std::chrono rust/jiff rust/chrono go/time java/java.time python/datetime python/whenever
localdatetime-arith 10000000 cpp/std::chrono rust/jiff rust/chrono go/time java/java.time python/datetime python/whenever
zoneddatetime-region 1000000 cpp/std::chrono rust/jiff go/time java/java.time python/zoneinfo
datetime-format-iso 1000000 cpp/std::chrono rust/jiff rust/chrono go/time java/java.time python/datetime python/whenever
datetime-parse-iso 1000000 cpp/date rust/jiff rust/chrono go/time java/java.time python/datetime python/whenever
'
IDS="$(awk 'NF{print $1}' <<<"$REQUIRED" | tr '\n' ' ')"

while IFS= read -r row; do
    [[ -z "$row" ]] && continue
    N="$(cawk '{print NF}' <<<"$row")"
    [[ "$N" == "$NCOLS" ]] || fail "row has $N cols, expected $NCOLS: $row"
    cawk '$1!="2" || $5!="time" || $33=="" || $34=="" || $37=="" || $37!=$10 {exit 1}' <<<"$row" \
        || fail "row is not v2 time/ with machine/os/platform: $row"
    b="$(cawk '{print $4}' <<<"$row")"
    case " $IDS " in *" $b "*) ;; *) fail "unexpected benchmark: $b" ;; esac
    st="$(cawk '{print $28}' <<<"$row")"
    if [[ "$st" == "skipped" ]]; then
        [[ -n "$(cawk '{print $30}' <<<"$row")" ]] || fail "skip row missing reason: $row"
    elif [[ "$st" != "ok" ]]; then
        fail "status '$st': $row"
    fi
done < <(csv_fields <<<"$OUT")

while read -r b ops libs; do
    [[ -z "$b" ]] && continue
    for want in $libs; do
        lang="${want%%/*}"; lib="${want#*/}"
        r="$(cawk -v b="$b" -v l="$lang" -v lib="$lib" '$4==b && $10==l && $12==lib' <<<"$OUT" | head -1)"
        [[ -n "$r" ]] || fail "no $lang $lib row for time/$b"
        [[ "$(cawk '{print $28 "/" $29}' <<<"$r")" == "ok/true" ]] \
            || fail "$lang $lib time/$b not ok/check_ok: $(cawk '{print $28, $29, $30}' <<<"$r")"
        min="$(cawk '{print $17}' <<<"$r")"
        (( min >= ops / 10 )) || fail "$lang $lib time/$b min_ns=$min for $ops operations looks elided"
    done
    echo "[time-comp] $b: $(cawk -v b="$b" '$4==b && $28=="ok" {printf "%s/%s ", $10, $12}' <<<"$OUT")"
done <<< "$REQUIRED"

echo "TIME-COMP OK"
