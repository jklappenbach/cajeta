#!/usr/bin/env bash
# gpu-matmul-profiling Unit 4 TDD smoke: the GPU competitor runner emits
# schema-valid (32-col) matmul/gpu rows; on-device ok rows are check_ok=true +
# backend=hip; an absent GPU venv degrades to skipped rows (exit 0, no crash).
set -euo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
PROJ="$( cd -- "${SCRIPT_DIR}/.." &> /dev/null && pwd )"
fail() { echo "GPU-COMPETITORS FAIL: $1" >&2; exit 1; }
cd "$PROJ"

# 4.1.a — PyTorch (python) rows are schema-valid; ok rows check out.
OUT=/tmp/gpu-competitors.csv
PROFILE_LANGS=python PROFILE_RUN_ID=gpu-comp bash competitors/gpu.sh > "$OUT" 2>/dev/null \
    || fail "gpu.sh (python) crashed"
rows="$(awk -F, '$4=="matmul"{print}' "$OUT")"
[[ -n "$rows" ]] || fail "no matmul rows emitted"
n_ok=0
while IFS= read -r r; do
    [[ -n "$r" ]] || continue
    nf="$(printf '%s' "$r" | awk -F, '{print NF}')"
    [[ "$nf" -eq 32 ]] || fail "row has $nf cols, expected 32 (backend col?)"
    st="$(printf '%s' "$r" | awk -F, '{print $28}')"
    [[ "$st" == "ok" || "$st" == "skipped" ]] || fail "bad status '$st'"
    if [[ "$st" == "ok" ]]; then
        ck="$(printf '%s' "$r" | awk -F, '{print $29}')"
        be="$(printf '%s' "$r" | awk -F, '{print $32}')"
        [[ "$ck" == "true" ]] || fail "ok row check_ok=$ck"
        [[ "$be" == "hip" ]]  || fail "ok row backend=$be"
        n_ok=$((n_ok + 1))
    fi
done <<< "$rows"
echo "[gpu-comp] python rows schema-valid; on-device ok rows=$n_ok"

# 4.1.b — skip-clean: an absent GPU venv yields skipped rows, exit 0.
PROFILE_LANGS=python PROFILE_GPU_PY=/nonexistent/python PROFILE_RUN_ID=gpu-skip \
    bash competitors/gpu.sh > /tmp/gpu-comp-skip.csv 2>/dev/null \
    || fail "skip path crashed (should exit 0)"
skst="$(awk -F, '$4=="matmul"{print $28; exit}' /tmp/gpu-comp-skip.csv)"
[[ "$skst" == "skipped" ]] || fail "absent venv should skip (got '$skst')"
echo "[gpu-comp] skip-clean ok (status=$skst)"

echo "GPU-COMPETITORS OK"
