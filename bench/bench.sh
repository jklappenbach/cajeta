#!/usr/bin/env bash
# bench — the benchmark reference's driver (specs/benchmark-reference-spec.md
# §2.1, §9). Runs the pipeline:
#   1. fetch + checksum the reference datasets
#   2. build the harness --release (benchmarks are native AOT)
#   3. run the benchmarks -> <out>/results.csv
#   3b. (--competitors) run the cross-language competitor runners, appending rows
#   4. capture the host env block -> <out>/env.csv
# The site renders committed results (bench/results/<machine>/<run-id>/); this
# script no longer generates a report.
#
#   ./bench.sh                         # everything (Cajeta side)
#   ./bench.sh --area collection       # one catalog package
#   ./bench.sh --area collection/hashmap-int   # one catalog path
#   ./bench.sh --bench saxpy           # one benchmark by name
#   ./bench.sh --list                  # list benchmarks under their catalog paths (no run)
#   ./bench.sh --competitors           # also run other-language competitors
#   ./bench.sh --machine phoenix ...   # record the machine (CAJETA_BENCH_MACHINE)
#   ./bench.sh publish <machine> <run-id>   # unit 22 (not yet implemented)
#
# Env: CAJETA= (compiler, default <repo>/build/src/cajeta), PROFILE_DATA_DIR=
# (dataset cache), PROFILE_OUT= (output dir, default build/results/<ts>),
# CAJETA_BENCH_SMOKE=1 (warmup 1, trials 3, rows tagged `smoke`, never published),
# CAJETA_BENCH_MACHINE= (same as --machine; defaults to the hostname).
set -euo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
REPO_ROOT="$( cd -- "${SCRIPT_DIR}/.." &> /dev/null && pwd )"
CAJETA="${CAJETA:-${REPO_ROOT}/build/src/cajeta}"
cd "$SCRIPT_DIR"

if [[ "${1:-}" == "publish" ]]; then
    echo "bench.sh publish: implemented in unit 22" >&2
    exit 2
fi

# Competitor runners (competitors/<runner>.sh) by catalog package. The runners
# still carry the profile-era area names; the mapping says which ones a
# package's --area selects.
competitor_runners() {
    case "$1" in
        "")         echo "codec hash sort string math collection clbg time stream concurrent gpu" ;;
        lang)       echo "string stream clbg math" ;;
        collection) echo "collection sort" ;;
        math)       echo "math" ;;
        xpu)        echo "gpu" ;;
        codec|hash|time|concurrent) echo "$1" ;;
        *)          echo "" ;;
    esac
}

[[ -x "$CAJETA" ]] || { echo "error: cajeta not found at $CAJETA (build ./build.sh or set CAJETA=)" >&2; exit 1; }

# Split our own flags (--competitors, --machine) from harness passthrough args;
# capture --area.
RUN_COMPETITORS=0
AREA_FILTER=""
LIST=0
MACHINE="${CAJETA_BENCH_MACHINE:-}"
HARNESS_ARGS=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --list) LIST=1; shift ;;
        --competitors) RUN_COMPETITORS=1; shift ;;
        --machine) MACHINE="${2:-}"; [[ -n "$MACHINE" ]] || { echo "error: --machine needs a name" >&2; exit 1; }; shift 2 ;;
        --area) AREA_FILTER="${2:-}"; HARNESS_ARGS+=("$1" "${2:-}"); shift 2 ;;
        *) HARNESS_ARGS+=("$1"); shift ;;
    esac
done
[[ -n "$MACHINE" ]] || MACHINE="$(hostname -s 2>/dev/null || hostname)"
export CAJETA_BENCH_MACHINE="$MACHINE"

# --list short-circuits (build + list, no run).
if [[ "$LIST" == "1" ]]; then
    "$CAJETA" release >/dev/null
    exec ./build/bench --list
fi

TS="$(date +%Y%m%d-%H%M%S)"
OUT="${PROFILE_OUT:-build/results/$TS}"
DATA="${PROFILE_DATA_DIR:-${SCRIPT_DIR}/datasets/cache}"
RUN_TS="$(date +%s)"
mkdir -p "$OUT"

echo "[bench] machine=$MACHINE${CAJETA_BENCH_SMOKE:+ (smoke mode: never published)}"
echo "[bench] (1/4) datasets -> $DATA"
PROFILE_DATA_DIR="$DATA" bash datasets/fetch.sh || \
    echo "  note: dataset fetch failed (offline?); dataset-backed benchmarks will record skips"

echo "[bench] (2/4) build --release"
"$CAJETA" release >/dev/null

echo "[bench] (3/4) run benchmarks -> $OUT/results.csv"
PROFILE_DATA_DIR="$DATA" PROFILE_RUN_ID="$TS" PROFILE_RUN_TS="$RUN_TS" \
    ./build/bench --run --out "$OUT/results.csv" ${HARNESS_ARGS[@]+"${HARNESS_ARGS[@]}"}

if [[ "$RUN_COMPETITORS" == "1" ]]; then
    echo "[bench] (3b) competitors (cross-language; builds cargo/cmake/go on first run)"
    for runner_name in $(competitor_runners "${AREA_FILTER%%/*}"); do
        runner="competitors/$runner_name.sh"
        [[ -f "$runner" ]] || continue
        echo "  [competitors] $runner_name"
        PROFILE_DATA_DIR="$DATA" PROFILE_RUN_ID="$TS" PROFILE_RUN_TS="$RUN_TS" \
            bash "$runner" >> "$OUT/results.csv" || echo "    note: $runner_name competitors failed (recorded as skips)"
    done
fi

echo "[bench] (4/4) capture env -> $OUT/env.csv"
bash scripts/env-capture.sh > "$OUT/env.csv"
echo "machine,${MACHINE//,/ }" >> "$OUT/env.csv"
# Stamp the Cajeta compiler version (the Cajeta side emits no language_version
# column of its own). Best-effort; never fail the run.
CAJETA_VER="$("$CAJETA" --version 2>/dev/null | head -1)"
[[ -n "$CAJETA_VER" ]] && echo "cajeta_version,${CAJETA_VER//,/ }" >> "$OUT/env.csv"
[[ -n "${CAJETA_BENCH_SMOKE:-}" ]] && echo "smoke,1" >> "$OUT/env.csv"

echo "[bench] done -> $OUT"
