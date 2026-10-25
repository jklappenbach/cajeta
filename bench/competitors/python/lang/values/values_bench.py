#!/usr/bin/env python3
# Python competitor: unit-5 lang/values benchmarks (contract: u5-competitor-spec.md).
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

import array


def main():
    n = sc(100000000)
    mask = MASK

    def optional():
        s = 0
        for i in range(n):
            o = (i & mask) if (i & 1) == 0 else None
            m = o * 2 if o is not None else None
            s += m if m is not None else 0
        return s
    half = (n + 1) // 2
    run("optional-map-unwrap", n, "None or int", PYVER, "no optional type: None or int", "", optional,
        lambda s: s == 4 * tri(half))

    def pair():
        s = 0
        for i in range(n):
            p = (i & 1023, i & mask)
            s += p[0] + p[1]
        return s
    run("pair-construct", n, "tuple", PYVER, "", "", pair, lambda s: s == block_sum(n, 1024) + tri(n))

    if selected("slice-window"):
        a = array.array("i", range(1000000))
        mv = memoryview(a)
        nw = sc(10000000)

        def window():
            s = 0
            for i in range(nw):
                st = i % 999936
                w = mv[st:st + 64]
                s += w[0]
            return s
        run("slice-window", nw, "memoryview slice", PYVER, "memoryview of array.array('i')", "", window,
            lambda s: s == block_sum(nw, 999936))
        mv.release()

    reps = sc(10)
    if selected("slice-iterate") or selected("arraystream-iterate"):
        a = array.array("i", (k % 1000 for k in range(10000000)))
        an = len(a)
        exp = reps * block_sum(an, 1000)

        def iterate():
            mv = memoryview(a)
            s = 0
            for _ in range(reps):
                for x in mv:
                    s += x
            mv.release()
            return s
        run("slice-iterate", reps * an, "memoryview", PYVER, "memoryview over array.array('i')", "",
            iterate, lambda s: s == exp)
        del a
        lst = [k % 1000 for k in range(10000000)]

        def stream():
            s = 0
            for _ in range(reps):
                s += sum(iter(lst))
            return s
        run("arraystream-iterate", reps * an, "sum(iter(list))", PYVER, "", "", stream, lambda s: s == exp)


if __name__ == "__main__":
    main()
