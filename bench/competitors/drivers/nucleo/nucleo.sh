#!/usr/bin/env bash
# nucleo/ competitors (benchmark-reference unit 12, spec §6.9): one Python
# program, competitors/python/nucleo/nucleo/nucleo_bench.py, runs every
# nucleo/ page id with the libraries the pages name, at the sizes and with the
# checks of the Cajeta side. Rows are v1 (31 columns); run.sh turns them into
# schema v2 with catalog ids nucleo/<slug>.
#
#   Table       pandas, polars, DuckDB     (db-benchmark port, datasets/gen_dbbench.py)
#   Column      pyarrow.compute, polars Series, NumPy
#   Tape        PyTorch autograd (CPU), JAX value_and_grad
#   Fuse        NumPy (unfused), numexpr, torch.compile, jax.jit
#   Transforms  JAX grad / vmap / jit, torch.func.grad / vmap, torch.compile
#
# Only Python competes here: the pages name Python libraries and no other
# language. The db-benchmark tables are generated on first use (seed 108) into
# $PROFILE_DATA_DIR/dbbench (default bench/datasets/cache): the 1e5 set under
# CAJETA_BENCH_SMOKE=1, 1e6 and 1e7 otherwise (1e7 only with 32 GB available).
# The GPU is never touched (CUDA_VISIBLE_DEVICES is emptied; JAX on CPU).
# A missing venv or library is a `skipped` row per id with the reason.
# Env: PROFILE_RUN_ID, PROFILE_RUN_TS, PROFILE_WARMUP, PROFILE_TRIALS,
#      PROFILE_DATA_DIR, NUCLEO_IDS (space list of slugs), NUCLEO_LIBS,
#      CAJETA_BENCH_SMOKE.
set -uo pipefail

DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )/../.." &> /dev/null && pwd )"   # competitors/
BENCH="$( cd -- "$DIR/.." &> /dev/null && pwd )"
LOGD="${TMPDIR:-/tmp}"
export PROFILE_DATA_DIR="${PROFILE_DATA_DIR:-${BENCH}/datasets/cache}"
strip() { tr -d ',"'; }
IDS="table-groupby table-join table-sort table-filter column-filter column-arith tape-forward-backward fuse-elementwise-chain transforms-grad transforms-vmap transforms-jit"

skip_all() {   # skip_all <reason>
    local rid="${PROFILE_RUN_ID:-local}" ts="${PROFILE_RUN_TS:-}" b
    for b in $IDS; do
        if [[ -n "${NUCLEO_IDS:-}" && " $NUCLEO_IDS " != *" $b "* ]]; then continue; fi
        echo "1,${rid},${ts},${b},nucleo,,0,,0,python,,python,,,0,0,-1,-1,-1,-1,,,-1,-1,-1,-1,-1,skipped,,$1,"
    done
}

PY="$DIR/python/.venv/bin/python"
if [[ ! -x "$PY" ]] || ! "$PY" -c 'import numpy' 2>/dev/null; then
    skip_all "venv with numpy missing (run competitors/setup.sh)"
    exit 0
fi

# The db-benchmark sets the Table rows read (cached by content; a no-op once made).
if [[ "${CAJETA_BENCH_SMOKE:-}" == 1 || "${CAJETA_BENCH_SMOKE:-}" == true ]]; then SIZES="1e5"; else SIZES="1e6 1e7"; fi
for n in $SIZES; do
    if [[ "$n" == 1e7 ]]; then
        avail="$(awk '/^MemAvailable:/ {print $2}' /proc/meminfo 2>/dev/null)"
        [[ -n "$avail" && "$avail" -lt $((32 * 1024 * 1024)) ]] && continue   # the 1e7 rows skip with the reason
    fi
    "$PY" "$BENCH/datasets/gen_dbbench.py" "$PROFILE_DATA_DIR" "$n" >&2 \
        || echo "nucleo.sh: gen_dbbench.py $n failed (the Table rows will skip)" >&2
done

PROFILE_LANG_VERSION="$("$PY" --version 2>&1 | strip)" "$PY" "$DIR/python/nucleo/nucleo/nucleo_bench.py" \
    2>"$LOGD/cajeta-nucleo-python.log" \
    || skip_all "python nucleo runner crashed (see $LOGD/cajeta-nucleo-python.log)"
exit 0
