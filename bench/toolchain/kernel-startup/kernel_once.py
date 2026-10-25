#!/usr/bin/env python3
"""One notebook-kernel launch for toolchain/kernel-startup (bench/toolchain/NotebookKernel.md).

    kernel_once.py --kernel cajeta --binary <cajeta>
    kernel_once.py --kernel ipython|ijava|evcxr [--python P] [--ijava-jar J] [--evcxr E]

The benchmark row times this whole process, once per trial: launch the kernel
through a kernelspec, wait for its `kernel_info` reply, run two cells, shut it
down. Every kernel is driven the same way, so the Python and jupyter_client
start-up this process pays is the same constant on every row.

The Cajeta launch is measure.py's own (`make_kernelspec`, `run_once`, its two
cells and its "is this really the cajeta kernel" assertion): this file wraps
that script, it does not re-implement it. The competitors go through the same
sequence with the same two cells written in their language (`a = 20`, then
`a + 22`, then `a + 1`), asserting the kernel's language before timing counts.

Prints one line `reply ok kernel=<k> startup=<s> cell1=<s> cell2=<s>` and exits
0 when every reply arrived with status ok; anything else exits 1 with the
reason on stderr.
"""
import argparse
import json
import os
import shutil
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import measure  # noqa: E402  (the original kernel-startup script; jupyter_client is required)
from jupyter_client.manager import KernelManager  # noqa: E402
from jupyter_client.kernelspec import KernelSpecManager  # noqa: E402

CELLS = {
    "ipython": ["a = 20\na + 22\n", "a + 1\n"],
    "ijava": ["int a = 20;\na + 22\n", "a + 1\n"],
    "evcxr": ["let a: i64 = 20;\na + 22\n", "a + 1\n"],
}
LANG = {"ipython": "python", "ijava": "java", "evcxr": "rust"}


def competitor_spec(kernel, args):
    root = Path(tempfile.mkdtemp(prefix="cajeta_bench_kspec_"))
    d = root / ("bench-" + kernel)
    d.mkdir()
    if kernel == "ipython":
        argv = [args.python, "-m", "ipykernel_launcher", "-f", "{connection_file}"]
    elif kernel == "ijava":
        argv = ["java", "-jar", args.ijava_jar, "{connection_file}"]
    else:
        argv = [args.evcxr, "--control_file", "{connection_file}"]
    (d / "kernel.json").write_text(json.dumps({
        "argv": argv, "display_name": kernel, "language": LANG[kernel],
        "interrupt_mode": "message"}))
    return root


def run_competitor(specroot, kernel, cwd, timeout):
    """measure.run_once's sequence for a competitor kernel."""
    ksm = KernelSpecManager()
    ksm.kernel_dirs = [str(specroot)]
    km = KernelManager(kernel_name="bench-" + kernel, kernel_spec_manager=ksm)
    timings = {}
    t0 = time.perf_counter()
    km.start_kernel(cwd=str(cwd))
    kc = km.client()
    kc.start_channels()
    try:
        kc.wait_for_ready(timeout=timeout)
        timings["startup"] = time.perf_counter() - t0
        kc.kernel_info()
        info = kc.get_shell_msg(timeout=timeout)["content"]
        lang = info.get("language_info", {}).get("name", "").lower()
        if lang != LANG[kernel]:
            raise RuntimeError("connected to a %r kernel, not %s" % (lang, LANG[kernel]))
        for i, code in enumerate(CELLS[kernel], start=1):
            t = time.perf_counter()
            msg_id = kc.execute(code)
            while True:
                reply = kc.get_shell_msg(timeout=timeout)
                if reply["parent_header"].get("msg_id") == msg_id:
                    break
            timings["cell%d" % i] = time.perf_counter() - t
            status = reply["content"].get("status")
            if status != "ok":
                timings["cell%d_status" % i] = status
                timings["cell%d_error" % i] = reply["content"].get("evalue", "")
    finally:
        kc.stop_channels()
        km.shutdown_kernel(now=True)
    return timings


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--kernel", required=True, choices=["cajeta", "ipython", "ijava", "evcxr"])
    ap.add_argument("--binary", help="the cajeta CLI (for --kernel cajeta)")
    ap.add_argument("--python", default=sys.executable)
    ap.add_argument("--ijava-jar")
    ap.add_argument("--evcxr")
    ap.add_argument("--timeout", type=float, default=600.0)
    args = ap.parse_args()

    cwd = Path(tempfile.mkdtemp(prefix="cajeta_bench_kcwd_"))
    if args.kernel == "cajeta":
        specroot = measure.make_kernelspec(Path(args.binary).resolve())
    else:
        specroot = competitor_spec(args.kernel, args)
    try:
        if args.kernel == "cajeta":
            t = measure.run_once(specroot, cwd, args.timeout)
        else:
            t = run_competitor(specroot, args.kernel, cwd, args.timeout)
    except Exception as e:  # noqa: BLE001 - the reason is the row's note
        print("kernel_once: %s: %s" % (args.kernel, e), file=sys.stderr)
        sys.exit(1)
    finally:
        shutil.rmtree(specroot, ignore_errors=True)
        shutil.rmtree(cwd, ignore_errors=True)
    for k in ("cell1_status", "cell2_status"):
        if k in t:
            print("kernel_once: %s %s=%s: %s" % (args.kernel, k, t[k],
                  str(t.get(k.replace("status", "error"), ""))[:200]), file=sys.stderr)
            sys.exit(1)
    if "cell2" not in t:
        print("kernel_once: %s: no reply to the second cell" % args.kernel, file=sys.stderr)
        sys.exit(1)
    print("reply ok kernel=%s startup=%.3f cell1=%.3f cell2=%.3f"
          % (args.kernel, t["startup"], t["cell1"], t["cell2"]), flush=True)


if __name__ == "__main__":
    main()
