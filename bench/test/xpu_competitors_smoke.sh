#!/usr/bin/env bash
# xpu/ and gfx/ competitor smoke (benchmark-reference unit 14, plan 14.1.2).
#
#  (1) build-only (PROFILE_BUILD_ONLY=1): competitors/xpu.sh and gfx.sh build
#      every device program -- the devserver (CUDA C++: cuBLAS, CUB, kernels,
#      texture objects; nvcc 13.3, sm_89), BabelStream v5.0 cuda-stream and
#      mixbench-cuda (pinned checkouts), and py_compile the PyTorch script --
#      and print a schema-v2 skipped row "build-only: built ..." for every id
#      each covers; nothing runs on a device. hip rows: "no ROCm on this
#      machine" where there is none. meshoptimizer (CPU) runs: ok, check_ok;
#  (2) every page id has a competitor row from the devserver;
#  (3) GPU held (CAJETA_BENCH_NO_GPU=1, no build-only): every device row is
#      skipped "GPU held by another session", and the PyTorch script never
#      imports torch (python -X importtime shows no torch module);
#  (4) CAJETA_BENCH_DEVICE=1 adds the device leg (every row ok or skipped,
#      ok rows check_ok) -- only on a quiet box.
# Env: CAJETA_BENCH_OUT (default /tmp).
set -euo pipefail
SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
source "$SCRIPT_DIR/csv_lib.sh"   # cawk / csv_fields: CSV-aware column checks
PROJ="$( cd -- "${SCRIPT_DIR}/.." &> /dev/null && pwd )"
OUTD="${CAJETA_BENCH_OUT:-/tmp}"; mkdir -p "$OUTD"
CHECK="python3 $SCRIPT_DIR/xpu_check.py"
HEADER="$(cat "$PROJ/competitors/columns.txt")"
fail() { echo "XPU-COMPETITORS FAIL: $1" >&2; exit 1; }
cd "$PROJ"

leg() {   # leg <tag> <env...> -> $OUTD/xpu-comp-<tag>.csv
    local tag="$1"; shift
    local csv="$OUTD/xpu-comp-$tag.csv" t0=$SECONDS
    { echo "$HEADER"
      env "$@" CAJETA_BENCH_SMOKE=1 PROFILE_RUN_ID="xpu-comp-$tag" PROFILE_TRIALS=3 bash competitors/xpu.sh
      env "$@" CAJETA_BENCH_SMOKE=1 PROFILE_RUN_ID="xpu-comp-$tag" PROFILE_TRIALS=3 bash competitors/gfx.sh
    } > "$csv" 2> "$OUTD/xpu-comp-$tag.err" || { cat "$OUTD/xpu-comp-$tag.err"; fail "competitors ($tag) exited non-zero"; }
    echo "[xpu-comp] leg $tag $((SECONDS - t0)) s"
    LEG="$csv"
}

# (1) + (2)
leg build PROFILE_BUILD_ONLY=1
$CHECK comp "$LEG" build | sed 's/^/[xpu-comp] /'
for id in $($CHECK ids | grep -v "^xpu/cross-backend-showcase$"); do   # its competitors are Cajeta's own backends
    python3 - "$LEG" "$id" <<'PY' || fail "page id $id has no devserver row"
import csv, sys
ok = any(r["area"] + "/" + r["benchmark"] == sys.argv[2] and r["library"].startswith("devserver") or
         (r["area"] + "/" + r["benchmark"] == sys.argv[2] and r["library"] == "meshoptimizer")
         for r in csv.DictReader(open(sys.argv[1])))
sys.exit(0 if ok else 1)
PY
done
echo "[xpu-comp] every page id has a competitor row (the showcase's are Cajeta's own backends)"
for lib in BabelStream mixbench torch meshoptimizer; do
    cawk -v l="$lib" 'NR>1 && $12==l' "$LEG" | grep -q . || fail "no $lib rows"
done

# (3)
leg held CAJETA_BENCH_NO_GPU=1
$CHECK comp "$LEG" held | tail -1 | sed 's/^/[xpu-comp] /'
PY="$PROJ/competitors/python/.venv/bin/python"; [[ -x "$PY" ]] || PY=python3
CAJETA_BENCH_NO_GPU=1 "$PY" -X importtime "$PROJ/competitors/python/xpu/torch_xpu/torch_xpu.py" >/dev/null 2>"$OUTD/xpu-comp-importtime.txt"
if grep -qE '\| +torch(\.|$)' "$OUTD/xpu-comp-importtime.txt"; then fail "torch was imported with the GPU held"; fi
echo "[xpu-comp] with the GPU held the PyTorch script imports no torch module"

# (4)
if [[ "${CAJETA_BENCH_DEVICE:-}" == 1 ]]; then
    leg device
    $CHECK comp "$LEG" run | tail -1 | sed 's/^/[xpu-comp] /'
else
    echo "[xpu-comp] device leg not run (CAJETA_BENCH_DEVICE=1 on a quiet box)"
fi
echo "XPU-COMPETITORS OK"
