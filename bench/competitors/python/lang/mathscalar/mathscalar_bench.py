#!/usr/bin/env python3
# Python competitor: unit-5 lang/mathscalar benchmarks (contract: u5-competitor-spec.md).
# ---- common harness (identical in every unit-5 python lang program) ----
# One v1-shape CSV row (32 columns) per (benchmark, variant, library). Each
# workload returns its result; the check is applied to every timed trial's
# result. PROFILE_PY_DIVISOR=d > 1 shrinks the outer repetition count (or N)
# by d for smoke runs; the expected value is derived for the scaled size and
# the row's notes gain `scaled 1/<d>`.
import os
import sys
import time


def env(k, d):
    v = os.environ.get(k)
    return v if v else d


RUN_ID = env("PROFILE_RUN_ID", "local")
TS = env("PROFILE_RUN_TS", "")
LANG_VERSION = env("PROFILE_LANG_VERSION", "")
WARMUP = max(0, int(env("PROFILE_WARMUP", "3")))
TRIALS = max(1, int(env("PROFILE_TRIALS", "10")))
DIV = max(1, int(env("PROFILE_PY_DIVISOR", "1")))
ONLY = [t for t in env("LANG_ONLY", "").split(",") if t]
FLAGS = "CPython 3.12"
PYVER = sys.version.split()[0]
# OPAQUE values: module globals derived at run time (CPython folds nothing
# across a loop anyway; kept for parity with the compiled competitors).
MASK = -1 - (len(sys.argv) - 1) * 0
ONE = 1 + (len(sys.argv) - 1) * 0


def sc(n):
    """Scale a count by PROFILE_PY_DIVISOR (minimum 1)."""
    return max(1, n // DIV)


def selected(slug):
    return not ONLY or slug in ONLY


def peak_rss_kb():
    try:
        with open("/proc/self/status") as f:
            for line in f:
                if line.startswith("VmHWM:"):
                    return int(line.split()[1])
    except OSError:
        pass
    return -1


def _notes(notes, scaled=True):
    parts = [p for p in notes.split(";") if p] if notes else []
    if scaled and DIV > 1:
        parts.append(f"scaled 1/{DIV}")
    return ";".join(parts).replace(",", ";")


def _row(slug, inp, lib, libver, mid, status, ok, notes, variant, scaled=True):
    return (f"1,{RUN_ID},{TS},{slug},lang,,{inp},,{inp},python,{LANG_VERSION},{lib},{libver},{FLAGS},"
            f"{WARMUP},{TRIALS},{mid},{peak_rss_kb()},-1,-1,-1,-1,{status},{ok},{_notes(notes, scaled)},{variant},cpu")


def emit(slug, inp, lib, libver, samples, ok, notes="", variant=""):
    s = sorted(samples)
    n = len(s)
    mn, med, mean, p95 = s[0], s[n // 2], sum(s) // n, s[min(n - 1, n * 95 // 100)]
    mops = (inp / med * 1e9 / 1e6) if med > 0 else 0.0
    mid = f"{mn},{med},{mean},{p95},{mops:.2f},Mop/s"
    print(_row(slug, inp, lib, libver, mid, "ok" if ok else "invalid", "true" if ok else "false",
               notes, variant), flush=True)


def emit_skipped(slug, inp, lib, libver, reason, variant=""):
    if not selected(slug):
        return
    print(_row(slug, inp, lib, libver, "-1,-1,-1,-1,,Mop/s", "skipped", "", reason, variant, scaled=False), flush=True)


def run(slug, inp, lib, libver, notes, variant, f, check):
    """warmup untimed runs, then timed trials; check every trial's result."""
    if not selected(slug):
        return
    for _ in range(WARMUP):
        check(f())
    samples = []
    ok = True
    for _ in range(TRIALS):
        t0 = time.perf_counter_ns()
        r = f()
        samples.append(time.perf_counter_ns() - t0)
        if not check(r):
            ok = False
    emit(slug, inp, lib, libver, samples, ok, notes, variant)


def tri(n):
    """sum of i for i in [0, n)"""
    return n * (n - 1) // 2


def block_sum(n, m):
    """sum of (i % m) for i in [0, n)"""
    q, r = divmod(n, m)
    return q * tri(m) + tri(r)
# ---- end common harness ----

import math


def main():
    if selected("math-transcendental"):
        import numpy as np
        m = sc(10000000)
        x = (np.arange(m, dtype=np.int64) % 1000).astype(np.float64) * 0.001
        if DIV > 1:
            blk = [math.sin(k * 0.001) + math.exp(k * 0.001) + math.sqrt(k * 0.001) for k in range(1000)]
            q, r = divmod(m, 1000)
            ref = q * math.fsum(blk) + math.fsum(blk[:r])
        else:
            ref = 28428598.85459962

        def transcendental():
            return float(np.sum(np.sin(x) + np.exp(x) + np.sqrt(x)))
        run("math-transcendental", m, "numpy", np.__version__, "vectorized; np.sum is pairwise", "",
            transcendental, lambda s: abs(s - ref) <= 1e-9 * ref)

    n = sc(100000000)

    def intdiv():
        s = 0
        for i in range(n):
            d = (i & 7) + 1
            s += i // d + i % d  # i >= 0 so floor == truncation
        return s
    if DIV > 1:  # plain untimed reference for the scaled size
        exp_div = sum(i // ((i & 7) + 1) + i % ((i & 7) + 1) for i in range(n))
    else:
        exp_div = 1698660847083334
    run("math-intdiv", n, "// %", PYVER, "", "", intdiv, lambda s: s == exp_div)

    def intrinsics():
        s = 0.0
        for i in range(n):
            a = float((i & 1023) - 512)
            s += abs(a) + min(a, 100.0) + max(a, -100.0) + (a * 0.5 + 1.0)
        return s
    terms = []
    for r in range(1024):
        a = float(r - 512)
        terms.append(abs(a) + min(a, 100.0) + max(a, -100.0) + (a * 0.5 + 1.0))
    q, r = divmod(n, 1024)
    exp_int = q * sum(terms) + sum(terms[:r])
    run("math-intrinsics", n, "abs/min/max", PYVER, "math.fma needs 3.13", "", intrinsics,
        lambda s: s == exp_int)


if __name__ == "__main__":
    main()
