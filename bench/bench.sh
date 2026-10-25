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

# Competitor rows come from competitors/<package>.sh, one per catalog package
# (schema v2, catalog ids; see competitors/run.sh). A filtered run runs its
# package's; an unfiltered run runs all of them.
PACKAGES="lang collection concurrent time hash codec io math nucleo wire search process reflect aot xpu gfx toolchain"
competitor_packages() {
    if [[ -z "$1" ]]; then echo "$PACKAGES"; else echo "$1"; fi
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
[[ -n "$MACHINE" ]] || MACHINE="$(cat /proc/sys/kernel/hostname 2>/dev/null || hostname)"
MACHINE="$(printf '%s' "$MACHINE" | tr -d '[:space:]' | tr 'A-Z' 'a-z')"
export CAJETA_BENCH_MACHINE="$MACHINE"

# --list short-circuits (build + list, no run). dev.cajeta.bench prints each
# benchmark as `<area>/<name>  [backend]`.
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
CAJETA_VER="$("$CAJETA" --version 2>/dev/null | head -1 || true)"
# dev.cajeta.bench writes results.csv and env.csv; it exits 1 when a row failed
# or was invalid (the run itself continues), so record that and carry on.
RC=0
PROFILE_DATA_DIR="$DATA" CAJETA_BENCH_RUN_ID="$TS" CAJETA_BENCH_RUN_TS="$RUN_TS" \
    CAJETA_BENCH_CAJETA_VERSION="$(printf '%s' "$CAJETA_VER" | awk '{print $2}')" \
    ./build/bench --run --out "$OUT/results.csv" ${HARNESS_ARGS[@]+"${HARNESS_ARGS[@]}"} || RC=$?

if [[ "$RUN_COMPETITORS" == "1" ]]; then
    echo "[bench] (3b) competitors (cross-language; builds cargo/cmake/go on first run)"
    for pkg in $(competitor_packages "${AREA_FILTER%%/*}"); do
        runner="competitors/$pkg.sh"
        [[ -f "$runner" ]] || continue
        echo "  [competitors] $pkg"
        PROFILE_DATA_DIR="$DATA" PROFILE_RUN_ID="$TS" PROFILE_RUN_TS="$RUN_TS" \
            bash "$runner" >> "$OUT/results.csv" || echo "    note: $pkg competitors failed (recorded as skips)"
    done
fi

echo "[bench] (4/4) env -> $OUT/env.csv"
# The harness wrote the run block (machine, os, kernel, cpu, cajeta_version,
# smoke); append the host block it cannot reach, without repeating a key.
touch "$OUT/env.csv"
bash scripts/env-capture.sh | awk -F, 'NR==FNR {seen[$1]=1; next} !($1 in seen)' "$OUT/env.csv" - >> "$OUT/env.csv"
[[ -n "$CAJETA_VER" ]] && ! grep -q '^cajeta_build,' "$OUT/env.csv" && echo "cajeta_build,${CAJETA_VER//,/ }" >> "$OUT/env.csv"

# Fidelity gate (spec §1.5.3). A full-catalog run must hold every benchmark the
# gate knows (--require-suite); a filtered run checks what it holds. Smoke rows
# carry no meaningful numbers, so a smoke run is not gated.
if [[ -z "${CAJETA_BENCH_SMOKE:-}" ]] && command -v python3 >/dev/null; then
    GATE_ARGS=()
    [[ "${#HARNESS_ARGS[@]}" == "0" ]] && GATE_ARGS+=(--require-suite)
    echo "[bench] fidelity gate ${GATE_ARGS[*]:-}"
    python3 scripts/verify-fidelity.py ${GATE_ARGS[@]+"${GATE_ARGS[@]}"} "$OUT/results.csv" || RC=1
fi

echo "[bench] done -> $OUT"
exit "$RC"
