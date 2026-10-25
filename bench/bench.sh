#!/usr/bin/env bash
# bench — the benchmark reference's driver (specs/benchmark-reference-spec.md
# §2.1, §9). Runs the pipeline:
#   0. (a non-smoke run) the quiet-box check of spec §9.1
#   1. fetch + checksum the reference datasets, and generate the ones that are
#      generated (dbbench tables, the codec CSV, the io files, the wire sources)
#   2. build the harness --release (benchmarks are native AOT)
#   3. run the benchmarks -> <out>/results.csv
#   3b. (--competitors) run the cross-language competitor runners, appending rows
#   4. capture the host env block and the quiet-box evidence -> <out>/env.csv
# The site renders committed results (bench/results/<machine>/<run-id>/); this
# script no longer generates a report.
#
#   ./bench.sh                         # everything (Cajeta side)
#   ./bench.sh --area collection       # one catalog package
#   ./bench.sh --area collection/hashmap-int   # one catalog path
#   ./bench.sh --bench saxpy           # one benchmark by name
#   ./bench.sh --list                  # list benchmarks under their catalog paths (no run)
#   ./bench.sh --registered-ids        # the ids of every build (registered-ids.txt's body):
#                                      # --list of the release build, plus the ids only the
#                                      # release-live-set-off build names (lang/alloc-live-set-off)
#   ./bench.sh --competitors           # also run other-language competitors
#   ./bench.sh --machine phoenix ...   # record the machine (default: the hostname, lowercased)
#   ./bench.sh --no-gpu ...            # CAJETA_BENCH_NO_GPU=1: no driver probe, no GPU check
#   ./bench.sh --force ...             # run on a box that fails the quiet-box check (recorded)
#   ./bench.sh --dry-run ...           # print the plan (plan: key=value) and the
#                                      # quiet-box evidence; build and run nothing
#   ./bench.sh publish <machine> <run-id>   # collect every repo's rows (spec §9.4)
#
# Quiet box (spec §9.1). A run that is not a smoke refuses to start (exit 3)
# unless the 1-minute load average is under 1, no CI runner job
# (Runner.Worker) is running, and the GPU shows 0 % utilization and no compute
# process (nvidia-smi, else rocm-smi; neither present reads "none"). The check
# runs before anything else and again after the build, which loads the box:
# the second one waits up to BENCH_QUIET_WAIT seconds (default 300) for the
# load to settle. The evidence at measurement start goes to env.csv as
# quiet_* lines. --no-gpu probes no GPU (quiet_gpu_* read "not probed").
#
# Env: CAJETA= (compiler, default <repo>/build/src/cajeta), PROFILE_DATA_DIR=
# (dataset cache), PROFILE_OUT= (output dir, default build/results/<ts>),
# CAJETA_BENCH_SMOKE=1 (warmup 1, trials 3, rows tagged `smoke`, never published),
# CAJETA_BENCH_MACHINE= (same as --machine), CAJETA_BENCH_IO_DIR= (io files,
# default build/io-data), BENCH_LOADAVG_FILE= (default /proc/loadavg; tests).
set -euo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
REPO_ROOT="$( cd -- "${SCRIPT_DIR}/.." &> /dev/null && pwd )"
CAJETA="${CAJETA:-${REPO_ROOT}/build/src/cajeta}"
cd "$SCRIPT_DIR"

# The fork's bin first: the compiler finds ld.lld only on PATH (the filed
# defect lld-lookup-path-only), and without it a --lto=thin release build
# silently drops LTO with a warning. ~/.cargo/bin for the Rust competitors.
FORK_BIN="${CAJETA_LLVM_BIN:-$( cd -- "${REPO_ROOT}/../cajeta-llvm/llvm-install/bin" 2>/dev/null && pwd || true )}"
[[ -d "$HOME/.cargo/bin" ]] && PATH="$HOME/.cargo/bin:$PATH"
[[ -n "$FORK_BIN" && -d "$FORK_BIN" ]] && PATH="$FORK_BIN:$PATH"
export PATH

if [[ "${1:-}" == "publish" ]]; then
    shift
    [[ -f "$SCRIPT_DIR/scripts/publish.sh" ]] || { echo "bench.sh publish: implemented in unit 22" >&2; exit 2; }
    exec bash "$SCRIPT_DIR/scripts/publish.sh" "$@"
fi

# Competitor rows come from competitors/<package>.sh, one per catalog package
# (schema v2, catalog ids; see competitors/run.sh). A filtered run runs its
# package's; an unfiltered run runs all of them.
PACKAGES="lang collection concurrent time hash codec io math nucleo wire search process reflect aot xpu gfx toolchain"
competitor_packages() {
    if [[ -z "$1" ]]; then echo "$PACKAGES"; else echo "$1"; fi
}

# Split our own flags from harness passthrough args; capture --area.
RUN_COMPETITORS=0
AREA_FILTER=""
FILTERED=0
LIST=0
DRY=0
FORCE=0
NO_GPU="${CAJETA_BENCH_NO_GPU:-}"
MACHINE="${CAJETA_BENCH_MACHINE:-}"
HARNESS_ARGS=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --list) LIST=1; shift ;;
        --registered-ids) LIST=2; shift ;;
        --dry-run) DRY=1; shift ;;
        --force) FORCE=1; shift ;;
        --no-gpu) NO_GPU=1; shift ;;
        --competitors) RUN_COMPETITORS=1; shift ;;
        --machine) MACHINE="${2:-}"; [[ -n "$MACHINE" ]] || { echo "error: --machine needs a name" >&2; exit 1; }; shift 2 ;;
        --area) AREA_FILTER="${2:-}"; FILTERED=1; HARNESS_ARGS+=("$1" "${2:-}"); shift 2 ;;
        --bench|--variant) FILTERED=1; HARNESS_ARGS+=("$1" "${2:-}"); shift 2 ;;
        --*) HARNESS_ARGS+=("$1"); shift ;;
        *) echo "error: unknown argument '$1' (bench.sh --help is the header of bench.sh)" >&2; exit 1 ;;
    esac
done
[[ -n "$MACHINE" ]] || MACHINE="$(cat /proc/sys/kernel/hostname 2>/dev/null || hostname)"
MACHINE="$(printf '%s' "$MACHINE" | tr -d '[:space:]' | tr 'A-Z' 'a-z')"
export CAJETA_BENCH_MACHINE="$MACHINE"
case "$NO_GPU" in 1|true|yes) NO_GPU=1; export CAJETA_BENCH_NO_GPU=1 ;; *) NO_GPU=0 ;; esac
SMOKE=0; case "${CAJETA_BENCH_SMOKE:-}" in 1|true|yes) SMOKE=1 ;; esac

# ---------------------------------------------------------------- quiet box
# quiet_probe: sets Q_LOAD, Q_CI, Q_GPU_UTIL, Q_GPU_PROCS and Q_WHY (the
# failed conditions, "; "-joined, empty when quiet).
quiet_probe() {
    Q_WHY=""
    Q_LOAD="$(awk '{print $1; exit}' "${BENCH_LOADAVG_FILE:-/proc/loadavg}" 2>/dev/null || echo unknown)"
    if [[ "$Q_LOAD" == "unknown" ]] || ! awk -v l="$Q_LOAD" 'BEGIN { exit !(l < 1) }'; then
        Q_WHY="${Q_WHY:+$Q_WHY; }load average $Q_LOAD is not under 1"
    fi
    Q_CI="$(ps -eo comm= 2>/dev/null | grep -cx 'Runner.Worker' || true)"
    [[ "${Q_CI:-0}" == "0" ]] || Q_WHY="${Q_WHY:+$Q_WHY; }$Q_CI CI runner job(s) active (Runner.Worker)"
    if [[ "$NO_GPU" == "1" ]]; then
        Q_GPU_UTIL="not probed (--no-gpu)"; Q_GPU_PROCS="not probed (--no-gpu)"
    elif command -v nvidia-smi >/dev/null 2>&1; then
        Q_GPU_UTIL="$(nvidia-smi --query-gpu=utilization.gpu --format=csv,noheader,nounits 2>/dev/null | tr -d ' ' | paste -sd/ -)"
        Q_GPU_PROCS="$(nvidia-smi --query-compute-apps=pid,process_name --format=csv,noheader 2>/dev/null | grep -c . || true)"
        [[ -n "$Q_GPU_UTIL" ]] || Q_GPU_UTIL="unknown"
        if ! printf '%s' "$Q_GPU_UTIL" | tr '/' '\n' | awk '!/^0$/ {bad=1} END {exit bad}'; then
            Q_WHY="${Q_WHY:+$Q_WHY; }GPU utilization $Q_GPU_UTIL % is not 0"
        fi
        [[ "${Q_GPU_PROCS:-0}" == "0" ]] || Q_WHY="${Q_WHY:+$Q_WHY; }$Q_GPU_PROCS GPU compute process(es) running"
    elif command -v rocm-smi >/dev/null 2>&1; then
        Q_GPU_UTIL="$(rocm-smi --showuse --csv 2>/dev/null | awk -F, 'NR>1 && $2 ~ /^[0-9]+$/ {print $2}' | paste -sd/ -)"
        Q_GPU_PROCS="$(rocm-smi --showpids --csv 2>/dev/null | awk -F, 'NR>1 && $1 ~ /^[0-9]+$/' | grep -c . || true)"
        [[ -n "$Q_GPU_UTIL" ]] || Q_GPU_UTIL="unknown"
        if ! printf '%s' "$Q_GPU_UTIL" | tr '/' '\n' | awk '!/^0$/ {bad=1} END {exit bad}'; then
            Q_WHY="${Q_WHY:+$Q_WHY; }GPU utilization $Q_GPU_UTIL % is not 0"
        fi
        [[ "${Q_GPU_PROCS:-0}" == "0" ]] || Q_WHY="${Q_WHY:+$Q_WHY; }$Q_GPU_PROCS GPU compute process(es) running"
    else
        Q_GPU_UTIL="none (no nvidia-smi or rocm-smi)"; Q_GPU_PROCS="none"
    fi
}
# quiet_evidence: the env.csv lines (values carry no comma: the load is a
# number, the reasons are "; "-joined).
quiet_evidence() {
    printf 'quiet_load_avg,%s\n' "$Q_LOAD"
    printf 'quiet_ci_jobs,%s\n' "$Q_CI"
    printf 'quiet_gpu_util,%s\n' "$Q_GPU_UTIL"
    printf 'quiet_gpu_procs,%s\n' "$Q_GPU_PROCS"
    printf 'quiet_check,%s\n' "$Q_STATE"
}
# quiet_gate <phase>: decide; exit 3 on a noisy box unless --force.
quiet_gate() {
    if [[ "$SMOKE" == "1" ]]; then Q_STATE="skipped (smoke)"; return 0; fi
    quiet_probe
    if [[ -z "$Q_WHY" ]]; then Q_STATE="pass"; return 0; fi
    if [[ "$FORCE" == "1" ]]; then
        Q_STATE="forced (--force at $1: $Q_WHY)"
        echo "[bench] quiet-box check failed at $1, continuing under --force: $Q_WHY" >&2
        return 0
    fi
    Q_STATE="refused ($1: $Q_WHY)"
    echo "[bench] refusing to start a published run ($1): $Q_WHY" >&2
    echo "[bench] (spec §9.1; a smoke run is not checked; --force overrides and records it in env.csv)" >&2
    quiet_evidence >&2
    exit 3
}
Q_STATE="skipped (smoke)"; Q_LOAD=""; Q_CI=""; Q_GPU_UTIL=""; Q_GPU_PROCS=""
[[ "$LIST" != "0" ]] || quiet_gate "start"

# ---------------------------------------------------------------- datasets
# Which generators this run needs: a full run all of them, a filtered one its
# package's. Sizes follow smoke mode (the benchmarks read the same scale).
PKG="${AREA_FILTER%%/*}"
needs() { [[ -z "$PKG" || "$PKG" == "$1" ]]; }
DATASETS="fetch"
if needs nucleo; then
    if [[ "$SMOKE" == "1" ]]; then DBB="1e5"; else DBB="1e6,1e7"; fi
    DATASETS="$DATASETS gen_dbbench:$DBB"
fi
if [[ "$SMOKE" == "1" ]]; then SCALE=smoke; else SCALE=full; fi
needs codec && DATASETS="$DATASETS gen_csv:$SCALE"
needs io && DATASETS="$DATASETS gen_io:$SCALE"
needs wire && DATASETS="$DATASETS gen_wire"

GATE_ARGS=()
[[ "$FILTERED" == "0" ]] && GATE_ARGS+=(--require-suite)

if [[ "$DRY" == "1" ]]; then
    echo "plan: machine=$MACHINE"
    echo "plan: path_head=${PATH%%:*}"
    echo "plan: ld_lld=$(command -v ld.lld || echo missing)"
    case ":$PATH:" in *":$HOME/.cargo/bin:"*) echo "plan: cargo_bin_on_path=yes" ;; *) echo "plan: cargo_bin_on_path=no" ;; esac
    echo "plan: no_gpu=$NO_GPU"
    echo "plan: smoke=$SMOKE"
    echo "plan: gate=${GATE_ARGS[*]:-}"
    echo "plan: datasets=$DATASETS"
    echo "plan: quiet=$Q_STATE"
    echo "plan: harness_args=${HARNESS_ARGS[*]:-}"
    [[ "$SMOKE" == "1" ]] || quiet_evidence
    exit 0
fi

[[ -x "$CAJETA" ]] || { echo "error: cajeta not found at $CAJETA (build ./build.sh or set CAJETA=)" >&2; exit 1; }

# --list short-circuits (build + list, no run). dev.cajeta.bench prints each
# benchmark as `<area>/<name>  [backend]`.
if [[ "$LIST" == "1" ]]; then
    "$CAJETA" release >/dev/null
    exec ./build/bench --list
fi
# --registered-ids: a benchmark whose id follows the build it is in
# (AllocLiveSetBench names alloc-live-set-on or -off from the live-set mode)
# lists under one id per binary, so take the union, first-seen order.
if [[ "$LIST" == "2" ]]; then
    "$CAJETA" release >/dev/null
    "$CAJETA" release-live-set-off >/dev/null
    { ./build/bench --list; ./build/bench-live-set-off --list; } | awk '!seen[$1]++'
    exit 0
fi

TS="$(date +%Y%m%d-%H%M%S)"
OUT="${PROFILE_OUT:-build/results/$TS}"
DATA="${PROFILE_DATA_DIR:-${SCRIPT_DIR}/datasets/cache}"
export CAJETA_BENCH_IO_DIR="${CAJETA_BENCH_IO_DIR:-${SCRIPT_DIR}/build/io-data}"
RUN_TS="$(date +%s)"
mkdir -p "$OUT" "$DATA" "$CAJETA_BENCH_IO_DIR"
VENV_PY="${SCRIPT_DIR}/competitors/python/.venv/bin/python"
[[ -x "$VENV_PY" ]] || VENV_PY="python3"

echo "[bench] machine=$MACHINE${CAJETA_BENCH_SMOKE:+ (smoke mode: never published)}${CAJETA_BENCH_NO_GPU:+ (no GPU)}"
echo "[bench] (1/4) datasets -> $DATA ($DATASETS)"
PROFILE_DATA_DIR="$DATA" bash datasets/fetch.sh || \
    echo "  note: dataset fetch failed (offline?); dataset-backed benchmarks will record skips"
for d in $DATASETS; do
    case "$d" in
        gen_dbbench:*)
            for n in $(printf '%s' "${d#gen_dbbench:}" | tr ',' ' '); do
                if [[ "$n" == "1e7" ]]; then
                    avail="$(awk '/MemAvailable/ {print $2; exit}' /proc/meminfo 2>/dev/null || echo 0)"
                    if [[ "${avail:-0}" -lt $((32 * 1024 * 1024)) ]]; then
                        echo "  note: dbbench 1e7 not generated (MemAvailable $((avail / 1048576)) GB < 32 GB; its rows skip)"; continue
                    fi
                fi
                "$VENV_PY" datasets/gen_dbbench.py "$DATA" "$n" >/dev/null \
                    || echo "  note: gen_dbbench.py $n failed (the Table rows will skip)"
            done ;;
        gen_csv:*) python3 datasets/gen_csv.py "$DATA" "${d#gen_csv:}" >/dev/null \
                    || echo "  note: gen_csv.py failed (the csv rows will skip)" ;;
        gen_io:*) python3 competitors/drivers/io/gen_io.py "$CAJETA_BENCH_IO_DIR" "${d#gen_io:}" >/dev/null \
                    || echo "  note: gen_io.py failed (the io file rows generate their own input)" ;;
        gen_wire)
            # The wire sources and schemas are committed; regenerating them in
            # place is a no-op unless the description moved, which git shows.
            python3 scripts/gen_wire.py "$SCRIPT_DIR" >/dev/null || { echo "error: gen_wire.py failed" >&2; exit 1; }
            if command -v git >/dev/null && [[ -n "$(git -C "$SCRIPT_DIR" status --porcelain -- src/main/cajeta/bench/wire competitors/cpp/wire/schema 2>/dev/null)" ]]; then
                echo "  note: gen_wire.py changed committed wire sources (git status); the run measures the regenerated ones"
            fi ;;
    esac
done

echo "[bench] (2/4) build --release"
"$CAJETA" release >/dev/null

# The build loaded the box: let it settle before measuring, and record the
# evidence as it stands when measurement starts.
if [[ "$SMOKE" == "0" ]]; then
    WAIT="${BENCH_QUIET_WAIT:-300}"; waited=0
    while :; do
        quiet_probe
        [[ -z "$Q_WHY" || "$waited" -ge "$WAIT" ]] && break
        sleep 10; waited=$((waited + 10))
    done
    quiet_gate "measurement start (after ${waited} s settling)"
fi

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
# smoke); append the host block it cannot reach and the quiet-box evidence,
# without repeating a key.
touch "$OUT/env.csv"
{ bash scripts/env-capture.sh; quiet_evidence; } | awk -F, 'NR==FNR {seen[$1]=1; next} !($1 in seen)' "$OUT/env.csv" - >> "$OUT/env.csv"
[[ -n "$CAJETA_VER" ]] && ! grep -q '^cajeta_build,' "$OUT/env.csv" && echo "cajeta_build,${CAJETA_VER//,/ }" >> "$OUT/env.csv"

# Fidelity gate (spec §1.5.3). A full-catalog run must hold every benchmark the
# gate knows (--require-suite); a filtered run checks what it holds. Smoke rows
# carry no meaningful numbers, so a smoke run is not gated.
if [[ "$SMOKE" == "0" ]] && command -v python3 >/dev/null; then
    echo "[bench] fidelity gate ${GATE_ARGS[*]:-}"
    python3 scripts/verify-fidelity.py ${GATE_ARGS[@]+"${GATE_ARGS[@]}"} "$OUT/results.csv" || RC=1
fi

echo "[bench] done -> $OUT"
exit "$RC"
