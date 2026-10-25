#!/usr/bin/env python3
"""Time-to-first-result for `cajeta kernel`, from a real Jupyter frontend.

Drives the kernel through `jupyter_client` — the same library Notebook and Lab
use — so what this reports is what a user sees, not what a C++ test harness
sees. The distinction is the whole point of this script: the kernel plan once
carried a latency figure taken from a Debug build of `cajeta_test`, which is
neither the shipped binary nor the shipped optimisation level.

THE NUMBER THAT MATTERS IS THE FIRST CELL, not startup. `KernelProtocol`
builds its session LAZILY, on the first `execute_request`, so the entire cost
of standing up a JIT session — priming the stdlib, resolving the project,
ingesting archives — lands inside cell 1 while the frontend shows a running
cell and nothing else. Startup-to-`kernel_info` is fast and says nothing about
this.

Scenarios, chosen so the comparison isolates the classpath:

  no-project         a notebook outside any project. Resident stdlib.
  project-no-deps    a project whose manifest declares no dependencies. MUST
                     match no-project: an empty classpath keeps the resident
                     path (KernelSession takes the reuse core only when there
                     are no archives), so any gap here is a defect, not a cost.
  project-with-deps  a real project with a real dependency. Fresh stdlib plus
                     archive ingest — the case worth quoting.

Usage:
    ./measure.py --binary ../../build-release/src/cajeta
    ./measure.py --binary ... --repeat 3 --scenario project-with-deps

No thresholds and no pass/fail: this prints numbers for review.
"""

import argparse
import json
import os
import shutil
import statistics
import sys
import tempfile
import time
from pathlib import Path

try:
    from jupyter_client.manager import KernelManager
    from jupyter_client.kernelspec import KernelSpecManager
except ImportError:
    sys.exit("jupyter_client is required: pip install jupyter_client")

# A project with exactly one dependency that resolves offline (the sibling
# cajeta-ml checkout). Overridable — the point is "a real project", not this one.
DEFAULT_DEPS_PROJECT = "/home/julian/code/cpp/cajeta-timeseries"

# Two cells. The first pays for the session; the second is the steady state,
# and the gap between them is the cost being measured.
CELLS = ["int32 a = 20;\na + 22;\n", "a + 1;\n"]

MINIMAL_MANIFEST = {
    "details": {
        "name": "dev.cajeta.benchempty",
        "version": "0.1.0",
        "description": "empty project for the kernel startup benchmark",
        "license": "Apache-2.0",
        "authors": ["bench"],
        "cajeta-lang-version": "1.0",
    },
    "settings": {
        "capabilities": [],
        "dependencies": {},
        "build": {"source-root": "src/main/cajeta", "target": "host"},
    },
}


def make_kernelspec(binary):
    """A throwaway kernelspec pointing at `binary`.

    Through a SPEC, not `KernelManager.kernel_cmd`. jupyter_client 8 ignores
    kernel_cmd on a KernelManager built without a spec and silently launches
    the DEFAULT kernel instead — this benchmark's first run cheerfully
    measured IPython and reported `invalid syntax (2735859215.py, line 1)`.
    Launching through a spec is also what a real frontend does, so it is the
    faithful path as well as the working one.
    """
    root = Path(tempfile.mkdtemp(prefix="cajeta_bench_spec_"))
    d = root / "cajeta-bench"
    d.mkdir()
    (d / "kernel.json").write_text(json.dumps({
        "argv": [str(binary), "kernel", "-f", "{connection_file}"],
        "display_name": "Cajeta (bench)",
        "language": "cajeta",
        "interrupt_mode": "message",
    }))
    return root


def run_once(specroot, cwd, timeout):
    """Launch a kernel in `cwd`, run CELLS, return per-phase seconds."""
    ksm = KernelSpecManager()
    ksm.kernel_dirs = [str(specroot)]
    km = KernelManager(kernel_name="cajeta-bench", kernel_spec_manager=ksm)
    timings = {}

    t0 = time.perf_counter()
    km.start_kernel(cwd=str(cwd))
    kc = km.client()
    kc.start_channels()
    try:
        kc.wait_for_ready(timeout=timeout)
        timings["startup"] = time.perf_counter() - t0

        # WHOSE KERNEL IS THIS? Assert before timing anything. A benchmark
        # that measures the wrong process is worse than no benchmark: it
        # produces confident numbers about software it never ran.
        kc.kernel_info()
        info = kc.get_shell_msg(timeout=timeout)["content"]
        impl = info.get("implementation", "")
        if impl != "cajeta":
            raise RuntimeError(
                f"connected to a {impl!r} kernel, not cajeta — "
                f"the kernelspec did not take")

        for i, code in enumerate(CELLS, start=1):
            t = time.perf_counter()
            msg_id = kc.execute(code)
            while True:
                reply = kc.get_shell_msg(timeout=timeout)
                if reply["parent_header"].get("msg_id") == msg_id:
                    break
            timings[f"cell{i}"] = time.perf_counter() - t
            status = reply["content"].get("status")
            if status != "ok":
                timings[f"cell{i}_status"] = status
                timings[f"cell{i}_error"] = reply["content"].get("evalue", "")
    finally:
        kc.stop_channels()
        km.shutdown_kernel(now=True)
    return timings


def scenario_dirs(deps_project):
    """Yield (name, path, cleanup) for each scenario."""
    empty = Path(tempfile.mkdtemp(prefix="cajeta_bench_noproject_"))
    yield "no-project", empty, lambda: shutil.rmtree(empty, ignore_errors=True)

    proj = Path(tempfile.mkdtemp(prefix="cajeta_bench_nodeps_"))
    (proj / "cajeta.json").write_text(json.dumps(MINIMAL_MANIFEST, indent=2))
    yield "project-no-deps", proj, lambda: shutil.rmtree(proj, ignore_errors=True)

    if deps_project and Path(deps_project).is_dir():
        yield "project-with-deps", Path(deps_project), lambda: None
    else:
        print(f"  (skipping project-with-deps: {deps_project} not found)",
              file=sys.stderr)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", required=True,
                    help="the cajeta CLI to measure (use a RELEASE build)")
    ap.add_argument("--repeat", type=int, default=3)
    ap.add_argument("--timeout", type=float, default=900.0)
    ap.add_argument("--deps-project", default=DEFAULT_DEPS_PROJECT)
    ap.add_argument("--scenario", action="append", default=None,
                    help="run only these scenarios (repeatable)")
    args = ap.parse_args()

    binary = Path(args.binary).resolve()
    if not binary.is_file() or not os.access(binary, os.X_OK):
        sys.exit(f"not an executable: {binary}")

    specroot = make_kernelspec(binary)
    print(f"binary : {binary}")
    print(f"repeat : {args.repeat}")
    print(f"NOTE   : cell 1 includes session creation — the session is built "
          f"lazily on first execute.\n")

    rows = []
    for name, path, cleanup in scenario_dirs(args.deps_project):
        if args.scenario and name not in args.scenario:
            cleanup()
            continue
        try:
            runs = []
            for r in range(args.repeat):
                print(f"  {name} [{r + 1}/{args.repeat}] ...",
                      end="", flush=True, file=sys.stderr)
                t = run_once(specroot, path, args.timeout)
                print(f" startup={t['startup']:.2f}s "
                      f"cell1={t.get('cell1', float('nan')):.2f}s",
                      file=sys.stderr)
                runs.append(t)
            rows.append((name, path, runs))
        finally:
            cleanup()

    def med(runs, key):
        vals = [r[key] for r in runs if key in r]
        return statistics.median(vals) if vals else float("nan")

    print(f"\n{'scenario':<20} {'startup':>10} {'cell 1':>10} {'cell 2':>10}"
          f"  {'first-cell overhead':>20}")
    print("-" * 76)
    baseline = None
    for name, path, runs in rows:
        c1 = med(runs, "cell1")
        if name == "no-project":
            baseline = c1
        over = "" if baseline is None else f"{c1 - baseline:+.2f}s"
        print(f"{name:<20} {med(runs, 'startup'):>9.2f}s {c1:>9.2f}s "
              f"{med(runs, 'cell2'):>9.2f}s  {over:>20}")
        for r in runs:
            for k in ("cell1_status", "cell2_status"):
                if k in r:
                    print(f"    ! {k}={r[k]}: {r.get(k.replace('status', 'error'), '')[:120]}")
    shutil.rmtree(specroot, ignore_errors=True)
    print(f"\nproject-with-deps source: {args.deps_project}")


if __name__ == "__main__":
    main()
