#!/usr/bin/env bash
# nucleo/ smoke (benchmark-reference unit 12, plan 12.1.1): every id on the
# nucleo pages (bench/nucleo/*.md) emits its rows under CAJETA_BENCH_SMOKE=1
# with status ok, check_ok true, a positive min_ns, the catalog id
# nucleo/<slug> and the smoke tag. The Table rows read the db-benchmark port's
# 1e5 set (datasets/gen_dbbench.py, seed 108; generated here when missing) for
# their 1e6 variants and check against DuckDB's stored answers; their 1e7
# variants are `skipped` with the smoke reason (they need 32 GB and are not
# smoke-sized). The one other skip allowed is tape-forward-backward, whose row
# names the nucleo defect that stops it (Tape.grow() dangles its grown arrays,
# so any tape past 32 nodes faults and would end the run); when that row turns
# ok (TapeBench.tapeGrows() flipped) it is checked like every other. Every check is the
# in-process one each benchmark states in its notes. Without PROFILE_DATA_DIR
# the Table rows skip with a reason instead of failing.
#
# Env: CAJETA (compiler, default <repo>/build/src/cajeta), CAJETA_BENCH_OUT
# (directory for the CSV and logs, default /tmp), PROFILE_DATA_DIR (default
# bench/datasets/cache), NO_BUILD=1 (reuse build/bench).
set -euo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
source "$SCRIPT_DIR/csv_lib.sh"   # cawk / csv_fields: CSV-aware column checks
PROJ="$( cd -- "${SCRIPT_DIR}/.." &> /dev/null && pwd )"
REPO_ROOT="$( cd -- "${PROJ}/.." &> /dev/null && pwd )"
CAJETA="${CAJETA:-${REPO_ROOT}/build/src/cajeta}"
OUTD="${CAJETA_BENCH_OUT:-/tmp}"; mkdir -p "$OUTD"
DATA="${PROFILE_DATA_DIR:-${PROJ}/datasets/cache}"
PY="${PROJ}/competitors/python/.venv/bin/python"

fail() { echo "NUCLEO FAIL: $1" >&2; exit 1; }
[[ -x "$CAJETA" ]] || fail "cajeta not found at $CAJETA"
[[ -x "$PY" ]] || fail "competitor venv missing (gen_dbbench.py needs numpy, polars, duckdb): run competitors/setup.sh"

cd "$PROJ"
if [[ "${NO_BUILD:-0}" != 1 ]]; then
    echo "[nucleo] building (release)..."
    t0=$SECONDS
    "$CAJETA" release >"$OUTD/nucleo-build.log" 2>&1 || { tail -30 "$OUTD/nucleo-build.log"; fail "release build failed"; }
    echo "[nucleo] build $((SECONDS - t0)) s"
fi

t0=$SECONDS
"$PY" "$PROJ/datasets/gen_dbbench.py" "$DATA" 1e5 >"$OUTD/nucleo-data.log" 2>&1 || { cat "$OUTD/nucleo-data.log"; fail "gen_dbbench.py 1e5 failed"; }
echo "[nucleo] dbbench 1e5 ready in $((SECONDS - t0)) s ($DATA/dbbench)"

OUT="$OUTD/cajeta-bench-nucleo.csv"
LOG="$OUTD/nucleo-run.log"
rm -f "$OUT"
t0=$SECONDS
CAJETA_BENCH_SMOKE=1 PROFILE_RUN_ID=nucleo-run PROFILE_DATA_DIR="$DATA" "$PROJ/build/bench" --run --area nucleo --out "$OUT" >"$LOG" 2>&1 \
    || { tail -30 "$LOG"; fail "--run --area nucleo exited non-zero"; }
echo "[nucleo] run $((SECONDS - t0)) s"

check() {   # check <slug> [variant]
    local b="$1" v="${2:-}" rows row
    rows="$(cawk -v b="$b" -v v="$v" 'NR>1 && $4==b && (v=="" || $31==v)' "$OUT")"
    [[ -n "$rows" ]] || fail "no row for nucleo/$b${v:+ [$v]}"
    while IFS= read -r row; do
        IFS="$CSV_FS" read -r -a c <<< "$row"
        [[ "${c[4]}/${c[3]}" == "nucleo/$b" ]] || fail "$b id=${c[4]}/${c[3]} (want nucleo/$b)"
        [[ "${c[27]}" == "ok" ]]   || fail "nucleo/$b${v:+ [$v]} status=${c[27]} notes=${c[29]}"
        [[ "${c[28]}" == "true" ]] || fail "nucleo/$b${v:+ [$v]} check_ok=${c[28]} notes=${c[29]}"
        [[ "${c[16]}" =~ ^[0-9]+$ && "${c[16]}" -gt 0 ]] || fail "nucleo/$b${v:+ [$v]} min_ns=${c[16]} (elided?)"
        [[ "${c[29]}" == *smoke* ]] || fail "nucleo/$b row not tagged smoke"
        echo "[nucleo] nucleo/$b${v:+ [$v]} ok  min_ns=${c[16]}"
    done <<< "$rows"
}
check_skip() {   # check_skip <slug> <variant>: skipped with the smoke reason
    local b="$1" v="$2" row
    row="$(cawk -v b="$b" -v v="$v" 'NR>1 && $4==b && $31==v' "$OUT")"
    [[ -n "$row" ]] || fail "no row for nucleo/$b [$v]"
    IFS="$CSV_FS" read -r -a c <<< "$row"
    [[ "${c[27]}" == "skipped" && "${c[29]}" == *"1e7 variant not run under CAJETA_BENCH_SMOKE=1"* ]] \
        || fail "nucleo/$b [$v] status=${c[27]} notes=${c[29]} (want the smoke skip)"
}

for b in table-groupby table-join; do
    for q in q1 q2 q3 q4 q5; do check "$b" "$q-1e6"; check_skip "$b" "$q-1e7"; done
done
for b in table-sort table-filter column-filter column-arith transforms-grad transforms-vmap transforms-jit; do
    check "$b"
done
trow="$(cawk 'NR>1 && $4=="tape-forward-backward"' "$OUT")"
if [[ "$(printf '%s' "$trow" | cawk '{print $28}')" == "skipped" ]]; then
    [[ "$trow" == *"Tape.grow()"* ]] || fail "tape-forward-backward skipped without the Tape.grow() reason: $trow"
    echo "[nucleo] nucleo/tape-forward-backward skipped (Tape.grow() defect, reason recorded)"
else
    check tape-forward-backward
fi
check fuse-elementwise-chain fused
check fuse-elementwise-chain unfused

# Only the 1e7 variants skip.
others="$(cawk 'NR>1 && $28=="skipped" && $31 !~ /-1e7$/ && $4 != "tape-forward-backward"' "$OUT" | wc -l)"
[[ "$others" == "0" ]] || fail "$others skipped rows besides the 1e7 variants and the tape defect"
# Every id on the pages has a row (a page row added later without a benchmark fails here).
ids="$(grep -ho '^| `nucleo/[a-z0-9-]*`' "$PROJ"/nucleo/*.md | sed 's/.*nucleo\///; s/`//' | sort -u)"
for id in $ids; do
    cawk -v b="$id" 'NR>1 && $4==b' "$OUT" | grep -q . || fail "page id nucleo/$id has no row"
done
others="$(cawk 'NR>1 && $5 != "nucleo"' "$OUT" | wc -l)"
[[ "$others" == "0" ]] || fail "--area nucleo emitted $others non-nucleo rows"

# No dataset: the Table rows skip with a reason, never fail.
OUT2="$OUTD/cajeta-bench-nucleo-nodata.csv"
CAJETA_BENCH_SMOKE=1 PROFILE_RUN_ID=nucleo-nd env -u PROFILE_DATA_DIR "$PROJ/build/bench" --run --bench table-filter --out "$OUT2" >>"$LOG" 2>&1 \
    || fail "table-filter without PROFILE_DATA_DIR exited non-zero"
cawk 'NR>1 && $4=="table-filter" && $28=="skipped" && $30 ~ /dbbench/' "$OUT2" | grep -q . \
    || fail "table-filter without PROFILE_DATA_DIR did not skip with the dataset reason"

echo "NUCLEO OK ($(echo "$ids" | wc -w) page ids)"
