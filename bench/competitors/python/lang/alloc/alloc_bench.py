#!/usr/bin/env python3
# Python competitor: unit-5 lang/alloc benchmarks (contract: u5-competitor-spec.md).
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

class Obj:
    __slots__ = ("a", "b", "c", "d")

    def __init__(self, v):
        self.a = v
        self.b = v
        self.c = v
        self.d = v


def pass_(o, depth):
    if depth == 0:
        return o.a
    return pass_(o, depth - 1)


def churn(n, r):
    ring = [None] * r
    s = 0
    for i in range(n):
        k = i % r
        ring[k] = Obj(i)  # previous occupant (oldest) released
        s += ring[k].a
    ringsum = sum(o.a for o in ring if o is not None)
    return s if ringsum >= 0 else -1


def main():
    n = sc(10000000)

    def small_object():
        s = 0
        for i in range(n):
            o = Obj(i)
            s += o.a
        return s
    run("alloc-small-object", n, "__slots__ class", PYVER, "refcount frees on rebind", "", small_object,
        lambda s: s == tri(n))

    run("alloc-churn-1", n, "list ring", PYVER, "", "", lambda: churn(n, 1), lambda s: s == tri(n))
    run("alloc-churn-10", n, "list ring", PYVER, "", "", lambda: churn(n, 10), lambda s: s == tri(n))

    t = sc(1000000)

    def transfer():
        s = 0
        for i in range(t):
            s += pass_(Obj(i), 10)
        return s
    run("alloc-transfer-chain", t, "reference", PYVER, "no ownership transfer: a reference is passed", "",
        transfer, lambda s: s == tri(t))

    emit_skipped("alloc-live-set-on", 0, "", "", "a Cajeta compiler mode (--live-set); no competitor counterpart", "")
    emit_skipped("alloc-live-set-off", 0, "", "", "a Cajeta compiler mode (--live-set); no competitor counterpart", "")

    na = sc(100000)

    def alloc_array():
        s = 0
        for i in range(na):
            a = [0] * 1024
            a[0] = i
            a[1023] = 1
            s += a[0] + a[1023]
        return s
    run("alloc-array", na, "[0]*1024", PYVER, "", "", alloc_array, lambda s: s == tri(na) + na)

    ns = sc(100000000)
    mask = MASK

    def struct_value():
        s = 0
        for i in range(ns):
            p = (i, i & mask)
            s += p[0] + p[1]
        return s
    run("alloc-struct-value", ns, "tuple", PYVER, "tuple; heap-allocated", "", struct_value,
        lambda s: s == 2 * tri(ns))


if __name__ == "__main__":
    main()
