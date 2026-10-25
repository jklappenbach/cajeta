#!/usr/bin/env bash
# nucleo/ competitor smoke (benchmark-reference unit 12, plan 12.1.2):
# competitors/run.sh nucleo under CAJETA_BENCH_SMOKE=1 emits schema-v2 rows
# with catalog ids nucleo/<slug> for every id on the nucleo pages, from every
# library the pages name, each with status ok and check_ok true:
#   table-groupby, table-join  pandas, polars, duckdb   q1..q5 at the 1e6 label
#                              (the 1e5 set under smoke; 1e7 skipped, reason given)
#   table-sort, table-filter   pandas, polars, duckdb
#   column-filter/-arith       pyarrow, polars, numpy
#   tape-forward-backward      torch, jax
#   fuse-elementwise-chain     numpy [unfused], numexpr, torch, jax [fused]
#   transforms-grad/-vmap/-jit jax, torch
# The Table rows read the same generated tables (datasets/gen_dbbench.py, seed
# 108) and check against the same stored DuckDB answers as the Cajeta side.
# Env: CAJETA_BENCH_OUT (log directory, default /tmp), PROFILE_DATA_DIR
# (default bench/datasets/cache).
set -uo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
source "$SCRIPT_DIR/csv_lib.sh"   # cawk / csv_fields: CSV-aware column checks
PROJ="$( cd -- "${SCRIPT_DIR}/.." &> /dev/null && pwd )"
OUTD="${CAJETA_BENCH_OUT:-/tmp}"; mkdir -p "$OUTD"
export PROFILE_DATA_DIR="${PROFILE_DATA_DIR:-${PROJ}/datasets/cache}"
NCOLS="$(cawk '{print NF; exit}' "${PROJ}/competitors/columns.txt")"

fail() { echo "NUCLEO-COMP FAIL: $1" >&2; exit 1; }

echo "[nucleo-comp] running the nucleo competitors (smoke sizes)..."
t0=$SECONDS
OUT="$(cd "${PROJ}/competitors" && CAJETA_BENCH_SMOKE=1 PROFILE_RUN_ID=nucleo-comp PROFILE_RUN_TS=0 TMPDIR="$OUTD" \
        bash run.sh nucleo 2>"$OUTD/nucleo-comp.err")" || fail "run.sh nucleo exited non-zero (see $OUTD/nucleo-comp.err)"
echo "[nucleo-comp] run $((SECONDS - t0)) s"
[[ -n "$OUT" ]] || fail "run.sh nucleo produced no rows"
printf '%s\n' "$OUT" > "$OUTD/nucleo-comp.csv"

while IFS= read -r row; do
    [[ -z "$row" ]] && continue
    IFS="$CSV_FS" read -r -a c <<< "$row"
    [[ "${#c[@]}" -ge $((NCOLS - 1)) && "${c[0]}" == "2" && "${c[4]}" == "nucleo" ]] || fail "row not v2 nucleo: $row"
    [[ "${c[9]}" == "python" && "${c[36]:-}" == "python" ]] || fail "row language/platform not python: $row"
    if [[ "${c[27]}" == "skipped" ]]; then
        [[ "${c[30]}" == *-1e7 && "${c[29]}" == *"1e7 variant not run under CAJETA_BENCH_SMOKE=1"* ]] \
            || fail "unexpected skip ${c[11]} ${c[3]} [${c[30]}]: ${c[29]}"
        continue
    fi
    [[ "${c[27]}" == "ok" && "${c[28]}" == "true" ]] || fail "${c[11]} ${c[3]} [${c[30]}] status=${c[27]} check=${c[28]} notes=${c[29]}"
    [[ "${c[16]}" =~ ^[0-9]+$ && "${c[16]}" -gt 0 ]] || fail "${c[11]} ${c[3]} [${c[30]}] min_ns=${c[16]}"
done < <(csv_fields <<<"$OUT")

has() {   # has <lib> <slug> [variant]
    printf '%s\n' "$OUT" | cawk -v l="$1" -v b="$2" -v v="${3:-}" '$12==l && $4==b && (v=="" || $31==v) && $28=="ok" && $29=="true"' | grep -q . \
        || fail "no ok $1 nucleo/$2${3:+ [$3]} row"
}
for lib in pandas polars duckdb; do
    for b in table-groupby table-join; do
        for q in q1 q2 q3 q4 q5; do has "$lib" "$b" "$q-1e6"; done
    done
    has "$lib" table-sort; has "$lib" table-filter
done
for lib in pyarrow polars numpy; do has "$lib" column-filter; has "$lib" column-arith; done
for lib in torch jax; do
    has "$lib" tape-forward-backward
    has "$lib" fuse-elementwise-chain fused
    for b in transforms-grad transforms-vmap transforms-jit; do has "$lib" "$b"; done
done
has numpy fuse-elementwise-chain unfused
has numexpr fuse-elementwise-chain fused

# Every page id has competitor rows.
ids="$(grep -ho '^| `nucleo/[a-z0-9-]*`' "$PROJ"/nucleo/*.md | sed 's/.*nucleo\///; s/`//' | sort -u)"
for id in $ids; do
    printf '%s\n' "$OUT" | cawk -v b="$id" '$4==b && $28=="ok"' | grep -q . || fail "page id nucleo/$id has no ok competitor row"
done
echo "[nucleo-comp] $(printf '%s\n' "$OUT" | cawk '$28=="ok"' | wc -l) ok rows across $(printf '%s\n' "$OUT" | cawk '$28=="ok"{print $12}' | sort -u | tr '\n' ' ')"
echo "NUCLEO-COMP OK"
