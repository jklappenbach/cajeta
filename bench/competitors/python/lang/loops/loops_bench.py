#!/usr/bin/env python3
# Python competitor: unit-5 lang/loops benchmarks (contract: u5-competitor-spec.md).
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


def nested_ref(a_n, b_n):
    """sum of i ^ j over i in [0,a_n) and j in [0,b_n), by bit counting"""
    def ones(n, b):  # count of x in [0,n) with bit b set
        p = 1 << (b + 1)
        return (n // p) * (1 << b) + max(0, n % p - (1 << b))
    t = 0
    for b in range(max(a_n, b_n).bit_length() + 1):
        oa, ob = ones(a_n, b), ones(b_n, b)
        t += (oa * (b_n - ob) + (a_n - oa) * ob) << b
    return t


def main():
    mask = MASK

    n = sc(100000000)

    def loop_counted():
        s = 0
        i = 0
        while i < n:
            s += i & mask
            i += 1
        return s
    run("loop-counted", n, "while", PYVER, "explicit counter in a while loop", "", loop_counted,
        lambda s: s == tri(n))

    reps = sc(100)
    if selected("loop-counted-nobounds"):
        a = list(range(1000000))
        an = len(a)

        def nobounds():
            s = 0
            for _ in range(reps):
                for k in range(an):
                    s += a[k]
            return s
        run("loop-counted-nobounds", reps * an, "list index", PYVER, "", "bounds-on", nobounds,
            lambda s: s == reps * tri(an))
        del a
    emit_skipped("loop-counted-nobounds", reps * 1000000, "list index", PYVER,
                 "CPython has no unchecked indexing", "bounds-off")

    outer = sc(10000)

    def nested():
        s = 0
        for i in range(outer):
            for j in range(10000):
                s += i ^ j
        return s
    run("loop-nested", outer * 10000, "for range", PYVER, "", "", nested,
        lambda s: s == nested_ref(outer, 10000))

    m = sc(10000000)
    if selected("loop-foreach-array") or selected("loop-foreach-arraylist"):
        arr = array.array("i", (k % 1000 for k in range(m)))

        def foreach_array():
            s = 0
            for x in arr:
                s += x
            return s
        run("loop-foreach-array", m, "array.array('i') for", PYVER, "", "", foreach_array,
            lambda s: s == block_sum(m, 1000))
        del arr
        lst = []
        for k in range(m):
            lst.append(k % 1000)

        def foreach_list():
            s = 0
            for x in lst:
                s += x
            return s
        run("loop-foreach-arraylist", m, "list for", PYVER, "list built by append", "", foreach_list,
            lambda s: s == block_sum(m, 1000))
        del lst

    def loop_range():
        s = 0
        for i in range(n):
            s += i & mask
        return s
    run("loop-range", n, "for range", PYVER, "", "", loop_range, lambda s: s == tri(n))

    if selected("loop-reduce"):
        x = [(k % 1000) * 0.5 for k in range(m)]
        ref = 0.5 * block_sum(m, 1000)

        def reduce_():
            s = 0.0
            for v in x:
                s += v
            return s
        run("loop-reduce", m, "for", PYVER, "sequential float add", "", reduce_,
            lambda s: abs(s - ref) <= 1e-6 * ref)
        del x

    er = sc(1000)
    if selected("loop-early-exit"):
        a = list(range(10000000))
        a[5000000] = -1
        an = len(a)

        def early_exit():
            s = 0
            for _ in range(er):
                for k in range(an):
                    if a[k] < 0:
                        s += k
                        break
            return s
        run("loop-early-exit", er * an, "for+break", PYVER, "list", "", early_exit,
            lambda s: s == er * 5000000)


if __name__ == "__main__":
    main()
