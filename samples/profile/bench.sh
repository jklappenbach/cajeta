#!/usr/bin/env bash
# profile — top-level driver (Unit 17). Runs the whole pipeline:
#   1. fetch + checksum the reference datasets
#   2. build the harness --release (benchmarks are native AOT)
#   3. run the benchmarks -> results/<timestamp>/results.csv
#   4. capture the host env block -> env.csv
#   5. generate the Cajeta-themed report site + report.md from the CSV
#
# Selection args pass through to the harness:
#   ./bench.sh                 # everything
#   ./bench.sh --area sort     # one area
#   ./bench.sh --bench saxpy   # one benchmark
#   ./bench.sh --list          # list benchmarks + areas (no run)
#
# Override the compiler with CAJETA=, the dataset cache with PROFILE_DATA_DIR=,
# and the output dir with PROFILE_OUT=.
set -euo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
REPO_ROOT="$( cd -- "${SCRIPT_DIR}/../.." &> /dev/null && pwd )"
CAJETA="${CAJETA:-${REPO_ROOT}/build-cajeta/src/cajeta}"
cd "$SCRIPT_DIR"

[[ -x "$CAJETA" ]] || { echo "error: cajeta not found at $CAJETA (build ./build.sh or set CAJETA=)" >&2; exit 1; }

# --list short-circuits (build + list, no run/report).
if [[ "${1:-}" == "--list" ]]; then
    "$CAJETA" release >/dev/null
    exec ./build/profile --list
fi

TS="$(date +%Y%m%d-%H%M%S)"
OUT="${PROFILE_OUT:-results/$TS}"
DATA="${PROFILE_DATA_DIR:-${SCRIPT_DIR}/datasets/cache}"
mkdir -p "$OUT"

echo "[bench] (1/5) datasets -> $DATA"
PROFILE_DATA_DIR="$DATA" bash datasets/fetch.sh || \
    echo "  note: dataset fetch failed (offline?); dataset-backed benchmarks will record skips"

echo "[bench] (2/5) build --release"
"$CAJETA" release >/dev/null

echo "[bench] (3/5) run benchmarks -> $OUT/results.csv"
PROFILE_DATA_DIR="$DATA" PROFILE_RUN_ID="$TS" PROFILE_RUN_TS="$(date +%s)" \
    ./build/profile --run --out "$OUT/results.csv" "$@"

echo "[bench] (4/5) capture env -> $OUT/env.csv"
bash scripts/env-capture.sh > "$OUT/env.csv"

echo "[bench] (5/5) report"
if command -v python3 >/dev/null; then
    python3 report/report.py "$OUT"
    ln -sfn "$(basename "$OUT")" results/latest 2>/dev/null || true
    echo "[bench] done -> open $OUT/site/index.html"
else
    echo "[bench] python3 not found — CSV written, report skipped"
fi
