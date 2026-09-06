#!/usr/bin/env bash
# xpubench — build and run the xpu-tile baseline harness (scheduling plan
# Unit 0), then render its rows with tools/xpubench-report.
#
#   test/xpu/bench/run.sh                 # gfx1151 + CPU backend, full shapes
#   test/xpu/bench/run.sh hip             # the GPU only
#   test/xpu/bench/run.sh cpu             # the CPU backend only
#   test/xpu/bench/run.sh smoke           # small shapes, both backends, quick
#
# Extra harness flags pass through after the leg name, e.g.
#   test/xpu/bench/run.sh hip --workloads=seam --blocks=3
#
# The harness refuses to run beside another cajeta_test / bench / GPU client
# (exit 3) — that is the idle gate, not a failure. Everything lands under the
# repo's tmp/bench (never /tmp): the binaries, the rows (one file per leg and
# date), the seam-probe profiler trace, and the rendered markdown.
#
# Env: CAJETA=<compiler> (default build/src/cajeta); LLM_DIR=<cajeta-llm
# checkout> and MODEL=<gguf> for the llm workload (default ../cajeta-llm and
# the Llama-3.1-8B Q4_K_M under ~/models); LLM=0 skips it.
set -euo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd -- "${HERE}/../../.." && pwd)"
CAJETA="${CAJETA:-${ROOT}/build/src/cajeta}"
OUT="${ROOT}/tmp/bench"
LLM_DIR="${LLM_DIR:-${ROOT}/../cajeta-llm}"
MODEL="${MODEL:-${HOME}/models/Meta-Llama-3.1-8B-Instruct-GGUF/Meta-Llama-3.1-8B-Instruct-Q4_K_M.gguf}"
LLM="${LLM:-1}"
GFX="${GFX:-gfx1151}"

[[ -x "$CAJETA" ]] || { echo "error: no cajeta at $CAJETA (build it, or set CAJETA=)" >&2; exit 2; }
mkdir -p "$OUT/arch" "$OUT/arch-report"

LEG="${1:-all}"
shift || true
EXTRA=("$@")

COMMIT="$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"
DATE="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
STAMP="$(date +%Y%m%d-%H%M)"
ROCM="$(hipconfig --version 2>/dev/null || true)"
KVER="$(uname -r)"
DRIVER_HIP="linux ${KVER}, ROCm ${ROCM:-unknown}"
DRIVER_CPU="linux ${KVER}"

echo "[xpubench] compiler $COMMIT at $CAJETA"

build_harness() {   # $1 = backends list, $2 = output name
    if [[ "${SKIP_BUILD:-0}" == "1" && -x "$OUT/$2" ]]; then echo "[xpubench] using prebuilt $OUT/$2"; return 0; fi
    echo "[xpubench] building $2 (--xpu-backend=$1)"
    "$CAJETA" --emit=exe --xpu-backend="$1" --xpu-arch="$GFX" --opt=O2 \
        -o "$OUT/$2" xpubench.Harness.main "$HERE/src" "$OUT/arch" \
        > "$OUT/build-$2.log" 2>&1 || { echo "BUILD FAILED — $OUT/build-$2.log:" >&2; tail -20 "$OUT/build-$2.log" >&2; exit 1; }
}

build_report() {
    if [[ "${SKIP_BUILD:-0}" == "1" && -x "$OUT/xpubench-report" ]]; then echo "[xpubench] using prebuilt xpubench-report"; return 0; fi
    echo "[xpubench] building xpubench-report"
    "$CAJETA" --emit=exe --opt=O2 -o "$OUT/xpubench-report" \
        xpubenchreport.Report.main "$ROOT/tools/xpubench-report/src" "$OUT/arch-report" \
        > "$OUT/build-report.log" 2>&1 || { echo "BUILD FAILED — $OUT/build-report.log:" >&2; tail -20 "$OUT/build-report.log" >&2; exit 1; }
}

build_llm() {   # the cajeta-llm SchedThroughput bench, GPU only (no cpu fallback)
    [[ "$LLM" == "1" ]] || return 0
    # a prebuilt bench (LLM_EXE=path) is reused as-is: rebuilding it is minutes
    # of compile on the box that is about to be measured
    if [[ -n "${LLM_EXE:-}" && -x "$LLM_EXE" ]]; then echo "[xpubench] using prebuilt $LLM_EXE"; return 0; fi
    [[ -d "$LLM_DIR/src/main/cajeta" ]] || { echo "[xpubench] no cajeta-llm at $LLM_DIR; llm workload will be pending"; return 0; }
    [[ -f "$MODEL" ]] || { echo "[xpubench] no model at $MODEL; llm workload will be pending"; return 0; }
    # cajeta-llm's declared dependencies (its cajeta.json): codec, jinja, logging
    local codec jinja logging
    codec="$(cd "$LLM_DIR/../cajeta-codec" 2>/dev/null && "$CAJETA" artifact-path 2>/dev/null | tail -1 || true)"
    jinja="$(cd "$LLM_DIR/../cajeta-jinja" 2>/dev/null && "$CAJETA" artifact-path 2>/dev/null | tail -1 || true)"
    logging="$(cd "$LLM_DIR/../cajeta-logging" 2>/dev/null && "$CAJETA" artifact-path 2>/dev/null | tail -1 || true)"
    [[ -f "$codec" && -f "$jinja" && -f "$logging" ]] || { echo "[xpubench] cajeta-codec / cajeta-jinja / cajeta-logging artifacts not found; llm workload will be pending"; return 0; }
    echo "[xpubench] building cajeta-llm SchedThroughput for amdgpu"
    mkdir -p "$OUT/arch-llm"
    CAJETA_OWNED_BIND=warn CAJETA_CAPTURED_BORROW=warn \
    "$CAJETA" --emit=exe --xpu-backend=amdgpu --xpu-arch="$GFX" --opt=O2 \
        --classpath="$codec,$jinja,$logging" -o "$OUT/schedthroughput" \
        dev.cajeta.llm.bench.SchedThroughput.run "$LLM_DIR/src/main/cajeta" "$OUT/arch-llm" \
        > "$OUT/build-llm.log" 2>&1 || { echo "[xpubench] cajeta-llm bench build failed — $OUT/build-llm.log; llm workload will be pending"; return 0; }
    LLM_EXE="$OUT/schedthroughput"
}

run_leg() {   # $1 = backend (hip|cpu), $2 = binary, $3.. = harness flags
    local backend="$1" bin="$2"; shift 2
    local rows="$OUT/rows-${backend}-${STAMP}.jsonl"
    local driver="$DRIVER_CPU"; [[ "$backend" == "hip" ]] && driver="$DRIVER_HIP"
    local llmflags=()
    if [[ "$backend" == "hip" && -n "${LLM_EXE:-}" ]]; then
        llmflags=(--llm-exe="$LLM_EXE" --model="$MODEL")
    fi
    echo "[xpubench] leg $backend -> $rows"
    rm -f "$rows"
    CAJETA_XPU_BACKEND="$backend" "$OUT/$bin" --out="$rows" --compiler="$COMMIT" \
        --driver="$driver" --date="$DATE" "${llmflags[@]}" "$@" "${EXTRA[@]}"
    # the seam probe again under the profiler: its launch-window slice is the
    # runtime's own view of the seam (cajeta profile summary --csv)
    if [[ "${SEAM_TRACE:-1}" == "1" ]]; then
        local trace="$OUT/seam-${backend}-${STAMP}.pftrace"
        rm -f "$trace"
        CAJETA_XPU_BACKEND="$backend" CAJETA_PROFILER=1 CAJETA_PROFILER_OUT="$trace" \
            "$OUT/$bin" --out="$OUT/seam-profiled-${backend}-${STAMP}.jsonl" --workloads=seam \
            --blocks=3 --compiler="$COMMIT" --driver="$driver" --date="$DATE" > /dev/null || true
        if [[ -s "$trace" ]]; then
            "$CAJETA" profile summary "$trace" --csv > "$OUT/seam-${backend}-${STAMP}.csv" 2>/dev/null || true
            echo "[xpubench] profiler launch-window per kernel (host tier unless rocprofiler-sdk is installed):"
            grep -E '^name|^spin' "$OUT/seam-${backend}-${STAMP}.csv" || true
        fi
    fi
    echo "[xpubench] rendering $OUT/baseline-${backend}-${STAMP}.md"
    "$OUT/xpubench-report" baseline "$rows" > "$OUT/baseline-${backend}-${STAMP}.md"
}

build_report
case "$LEG" in
    all)
        build_harness "amdgpu,cpu" xpubench
        build_llm
        run_leg hip xpubench
        run_leg cpu xpubench
        ;;
    hip)
        build_harness "amdgpu,cpu" xpubench
        build_llm
        run_leg hip xpubench
        ;;
    cpu)
        build_harness "cpu" xpubench-cpu
        run_leg cpu xpubench-cpu
        ;;
    smoke)
        build_harness "amdgpu,cpu" xpubench
        SEAM_TRACE=0 run_leg hip xpubench --shapes=small --blocks=2 --per=3 --frames=10 --workloads=kernels,cg,degenerate,frame,pair,seam
        SEAM_TRACE=0 run_leg cpu xpubench --shapes=small --blocks=2 --per=3 --frames=10 --workloads=kernels,cg,degenerate,frame,pair,seam
        ;;
    *)
        echo "usage: $0 [all|hip|cpu|smoke] [harness flags...]" >&2
        exit 2
        ;;
esac
echo "[xpubench] done"
