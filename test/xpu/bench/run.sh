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
# DATE and STAMP are overridable so a leg split across invocations (to fit a
# tool's timeout: `run.sh cpu --workloads=kernels`, then KEEP_ROWS=1
# DATE=... STAMP=... `run.sh cpu --workloads=cg,...`) keeps one identity and
# one rows file; KEEP_ROWS=1 appends to that file instead of truncating it.
DATE="${DATE:-$(date -u +%Y-%m-%dT%H:%M:%SZ)}"
STAMP="${STAMP:-$(date +%Y%m%d-%H%M)}"
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

# One profiled pass: the harness under CAJETA_PROFILER with a ring sized to
# the launch count, the trace summarised per kernel, and the rows the host
# clock cannot produce derived from the spans and appended to the leg's rows
# (xpu-tile-scheduling plan 0.2.4; the derivations live in
# tools/xpubench-report Spans.cajeta). A pass runs one shape and one seam
# target at a time so every span in the trace belongs to one row.
#   $1 backend  $2 binary  $3 leg rows  $4 driver  $5 tag  $6 ring  $7.. harness flags
profile_pass() {
    local backend="$1" bin="$2" rows="$3" driver="$4" tag="$5" ring="$6"; shift 6
    local base="$OUT/prof-${tag}-${backend}-${STAMP}"
    rm -f "$base.pftrace" "$base.jsonl" "$base.csv"
    echo "[xpubench] profiled pass $tag (ring $ring)"
    CAJETA_XPU_BACKEND="$backend" CAJETA_PROFILER=1 CAJETA_PROFILER_OUT="$base.pftrace" \
        CAJETA_PROFILER_GPU_RING="$ring" \
        "$OUT/$bin" --out="$base.jsonl" --compiler="$COMMIT" --driver="$driver" --date="$DATE" \
        "$@" > "$base.log" 2>&1 || { echo "[xpubench] profiled pass $tag failed — $base.log" >&2; return 0; }
    if [[ ! -s "$base.pftrace" ]]; then echo "[xpubench] no trace from pass $tag" >&2; return 0; fi
    "$CAJETA" profile summary "$base.pftrace" --csv > "$base.csv" 2>/dev/null || { echo "[xpubench] no device track in pass $tag" >&2; return 0; }
    "$OUT/xpubench-report" spans --csv="$base.csv" --rows="$base.jsonl" --out="$rows"
}

span_passes() {   # $1 backend  $2 binary  $3 leg rows  $4 driver
    local backend="$1" bin="$2" rows="$3" driver="$4"
    local wl
    wl="$(harness_workloads)"
    if has_workload "$wl" kernels; then
        profile_pass "$backend" "$bin" "$rows" "$driver" kernels-small 65536 \
            --workloads=kernels --kernel-shape=small --blocks=2 --per=5
        profile_pass "$backend" "$bin" "$rows" "$driver" kernels-large 65536 \
            --workloads=kernels --kernel-shape=large --blocks=2 --per=5
    fi
    if has_workload "$wl" cg; then
        profile_pass "$backend" "$bin" "$rows" "$driver" cg 262144 --workloads=cg --blocks=2
    fi
    if has_workload "$wl" degenerate; then
        profile_pass "$backend" "$bin" "$rows" "$driver" degenerate 262144 --workloads=degenerate --blocks=2
    fi
    if has_workload "$wl" seam; then
        local t
        for t in 5 50 200; do
            profile_pass "$backend" "$bin" "$rows" "$driver" "seam-$t" 65536 \
                --workloads=seam --seam-targets="$t" --blocks=2
        done
    fi
    # cajeta-llm: the bench process itself under the profiler (the harness
    # cannot hand its child an extended environment), its wall from the two
    # lines it prints, the rows derived with --llm.
    if [[ "$backend" == "hip" && -n "${LLM_EXE:-}" && "$LLM" == "1" ]] && has_workload "$wl" llm; then
        local base="$OUT/prof-llm-${backend}-${STAMP}"
        rm -f "$base.pftrace" "$base.csv" "$base.log"
        echo "[xpubench] profiled pass llm (ring 4194304)"
        CAJETA_PROFILER=1 CAJETA_PROFILER_OUT="$base.pftrace" CAJETA_PROFILER_GPU_RING=4194304 \
            "$LLM_EXE" "$MODEL" "prompt=${PROMPT:-2048}" "gen=${GEN:-64}" "ctx=$(( ${PROMPT:-2048} + ${GEN:-64} + 64 ))" \
            > "$base.log" 2>&1 || { echo "[xpubench] profiled llm run failed — $base.log" >&2; return 0; }
        local pre dec wall
        pre="$(awk '/^prefill ms:/ {print $3}' "$base.log" | head -1)"
        dec="$(awk '/^decode ms:/ {print $3}' "$base.log" | head -1)"
        if [[ -z "$pre" || -z "$dec" || ! -s "$base.pftrace" ]]; then echo "[xpubench] profiled llm run printed no timings" >&2; return 0; fi
        wall="$(awk -v a="$pre" -v b="$dec" 'BEGIN { printf "%d", a + b }')"
        "$CAJETA" profile summary "$base.pftrace" --csv > "$base.csv" 2>/dev/null || return 0
        "$OUT/xpubench-report" spans --llm --csv="$base.csv" --rows="$rows" --out="$rows" \
            --shape="prompt${PROMPT:-2048}+gen${GEN:-64}" --wall-ms="$wall"
    fi
}

# The --workloads= list this invocation runs (the harness default when the
# caller passed none), so the profiled passes cover the same set.
harness_workloads() {
    local a
    for a in "${EXTRA[@]}"; do
        case "$a" in --workloads=*) echo "${a#--workloads=}"; return 0;; esac
    done
    echo "kernels,cg,degenerate,frame,pair,seam,llm"
}
has_workload() { [[ ",$1," == *",$2,"* ]]; }

run_leg() {   # $1 = backend (hip|cpu), $2 = binary, $3.. = harness flags
    local backend="$1" bin="$2"; shift 2
    local rows="$OUT/rows-${backend}-${STAMP}.jsonl"
    local driver="$DRIVER_CPU"; [[ "$backend" == "hip" ]] && driver="$DRIVER_HIP"
    local llmflags=()
    if [[ "$backend" == "hip" && -n "${LLM_EXE:-}" ]]; then
        llmflags=(--llm-exe="$LLM_EXE" --model="$MODEL")
    fi
    echo "[xpubench] leg $backend -> $rows"
    [[ "${KEEP_ROWS:-0}" == "1" ]] || rm -f "$rows"
    # SPANS_ONLY=1 (with KEEP_ROWS=1 and the leg's DATE/STAMP) re-runs just
    # the profiled passes of the listed workloads and appends their rows.
    if [[ "${SPANS_ONLY:-0}" != "1" ]]; then
        CAJETA_XPU_BACKEND="$backend" "$OUT/$bin" --out="$rows" --compiler="$COMMIT" \
            --driver="$driver" --date="$DATE" "${llmflags[@]}" "$@" "${EXTRA[@]}"
    fi
    # the profiled passes: device spans → rows the host clock cannot give
    if [[ "${SPANS:-1}" == "1" ]]; then
        span_passes "$backend" "$bin" "$rows" "$driver"
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
