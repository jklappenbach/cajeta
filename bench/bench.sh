#!/usr/bin/env bash
# profile — top-level driver (Unit 17). Runs the whole pipeline:
#   1. fetch + checksum the reference datasets
#   2. build the harness --release (benchmarks are native AOT)
#   3. run the benchmarks -> results/<timestamp>/results.csv
#   3b. (--competitors) run the cross-language competitor runners, appending rows
#   4. capture the host env block -> env.csv
#   5. generate the Cajeta-themed report site + report.md from the CSV
#
# Selection args pass through to the harness:
#   ./bench.sh                      # everything (Cajeta side)
#   ./bench.sh --area sort          # one area
#   ./bench.sh --bench saxpy        # one benchmark
#   ./bench.sh --list               # list benchmarks + areas (no run)
#   ./bench.sh --competitors        # also run other-language competitors (Phase B)
#   ./bench.sh --area codec --competitors   # Cajeta + competitors, one area
#
# Override the compiler with CAJETA=, the dataset cache with PROFILE_DATA_DIR=,
# and the output dir with PROFILE_OUT=.
set -euo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
REPO_ROOT="$( cd -- "${SCRIPT_DIR}/../.." &> /dev/null && pwd )"
CAJETA="${CAJETA:-${REPO_ROOT}/build-cajeta/src/cajeta}"
cd "$SCRIPT_DIR"

# Areas that have a competitor runner (competitors/<area>.sh). Grows as Phase B lands.
COMPETITOR_AREAS="codec hash sort string math collection clbg time stream concurrent gpu"

[[ -x "$CAJETA" ]] || { echo "error: cajeta not found at $CAJETA (build ./build.sh or set CAJETA=)" >&2; exit 1; }

# --list short-circuits (build + list, no run/report).
if [[ "${1:-}" == "--list" ]]; then
    "$CAJETA" release >/dev/null
    exec ./build/profile --list
fi

# Split our own flags (--competitors) from harness passthrough args; capture --area.
RUN_COMPETITORS=0
AREA_FILTER=""
HARNESS_ARGS=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --competitors) RUN_COMPETITORS=1; shift ;;
        --area) AREA_FILTER="${2:-}"; HARNESS_ARGS+=("$1" "${2:-}"); shift 2 ;;
        *) HARNESS_ARGS+=("$1"); shift ;;
    esac
done

TS="$(date +%Y%m%d-%H%M%S)"
OUT="${PROFILE_OUT:-results/$TS}"
DATA="${PROFILE_DATA_DIR:-${SCRIPT_DIR}/datasets/cache}"
RUN_TS="$(date +%s)"
mkdir -p "$OUT"

echo "[bench] (1/5) datasets -> $DATA"
PROFILE_DATA_DIR="$DATA" bash datasets/fetch.sh || \
    echo "  note: dataset fetch failed (offline?); dataset-backed benchmarks will record skips"

echo "[bench] (2/5) build --release"
"$CAJETA" release >/dev/null

echo "[bench] (3/5) run benchmarks -> $OUT/results.csv"
PROFILE_DATA_DIR="$DATA" PROFILE_RUN_ID="$TS" PROFILE_RUN_TS="$RUN_TS" \
    ./build/profile --run --out "$OUT/results.csv" "${HARNESS_ARGS[@]}"

if [[ "$RUN_COMPETITORS" == "1" ]]; then
    echo "[bench] (3b) competitors (cross-language; builds cargo/cmake/go on first run)"
    for area in $COMPETITOR_AREAS; do
        [[ -n "$AREA_FILTER" && "$AREA_FILTER" != "$area" ]] && continue
        runner="competitors/$area.sh"
        [[ -f "$runner" ]] || continue
        echo "  [competitors] $area"
        PROFILE_DATA_DIR="$DATA" PROFILE_RUN_ID="$TS" PROFILE_RUN_TS="$RUN_TS" \
            bash "$runner" >> "$OUT/results.csv" || echo "    note: $area competitors failed (recorded as skips)"
    done
fi

echo "[bench] (4/5) capture env -> $OUT/env.csv"
bash scripts/env-capture.sh > "$OUT/env.csv"
# Stamp the Cajeta compiler version so the report's Languages legend can show it
# alongside the competitors' toolchain versions (the Cajeta side emits no
# language_version column of its own). Best-effort; never fail the run.
CAJETA_VER="$("$CAJETA" --version 2>/dev/null | head -1)"
[[ -n "$CAJETA_VER" ]] && echo "cajeta_version,${CAJETA_VER//,/ }" >> "$OUT/env.csv"

echo "[bench] (5/5) report"
if command -v python3 >/dev/null; then
    python3 report/report.py "$OUT"
    ln -sfn "$(basename "$OUT")" results/latest 2>/dev/null || true
    echo "[bench] done -> open $OUT/site/index.html"
else
    echo "[bench] python3 not found — CSV written, report skipped"
fi
