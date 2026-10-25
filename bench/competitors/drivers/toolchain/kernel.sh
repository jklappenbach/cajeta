#!/usr/bin/env bash
# toolchain/kernel-startup competitors (bench/toolchain/NotebookKernel.md): the
# IPython kernel (ipykernel), IJava and evcxr, each launched through a
# kernelspec by toolchain/kernel-startup/kernel_once.py (the wrapper around
# measure.py the Cajeta row uses), to its kernel_info reply and two cells.
# Each trial is one kernel_once.py process; the check is every reply ok.
# Pins: ipykernel and jupyter_client in python/requirements.txt, IJava 1.3.0's
# release zip and evcxr_jupyter 0.22.0 in pins.tsv (setup.sh provisions them).
set -uo pipefail
source "$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )/../../lib" &> /dev/null && pwd )/toolchain.sh"
K="$BENCH/toolchain/kernel-startup/kernel_once.py"
IJ="$TC/third_party/ijava/java/ijava-1.3.0.jar"
EV="$TC/third_party/evcxr/bin/evcxr_jupyter"
N="one launch to the kernel_info reply and two cells; the wrapper process is timed"

if "$PY" -c "import jupyter_client, ipykernel" 2>/dev/null; then
    IV="$("$PY" -c 'import importlib.metadata as m; print(m.version("ipykernel"))')"
    JV="$("$PY" -c 'import importlib.metadata as m; print(m.version("jupyter_client"))')"
    cb --bench kernel-startup --lang python --lang-version "$V_CPY" --library ipykernel --library-version "$IV" \
       --flags "jupyter_client $JV" --warmup 1 --trials 20 --expect-stdout "reply ok" --notes "$N" \
       -- "$PY" "$K" --kernel ipython --python "$PY"
    if [[ -f "$IJ" ]] && command -v java >/dev/null; then
        cb --bench kernel-startup --lang java --lang-version "$V_JAVA" --library IJava --library-version 1.3.0 \
           --flags "jupyter_client $JV" --warmup 1 --trials 20 --expect-stdout "reply ok" --notes "$N" \
           -- "$PY" "$K" --kernel ijava --ijava-jar "$IJ"
    else
        skip_row kernel-startup java IJava "IJava 1.3.0 not provisioned (setup.sh: third_party/ijava)"
    fi
    if [[ -x "$EV" ]]; then
        cb --bench kernel-startup --lang rust --lang-version "$V_RUSTC" --library evcxr --library-version 0.22.0 \
           --flags "jupyter_client $JV" --warmup 1 --trials 20 --expect-stdout "reply ok" --notes "$N" \
           -- "$PY" "$K" --kernel evcxr --evcxr "$EV"
    else
        skip_row kernel-startup rust evcxr "evcxr_jupyter 0.22.0 not provisioned (setup.sh: third_party/evcxr)"
    fi
else
    for l in "python ipykernel" "java IJava" "rust evcxr"; do
        skip_row kernel-startup ${l% *} ${l#* } "jupyter_client/ipykernel not in the venv (setup.sh)"
    done
fi
exit 0
