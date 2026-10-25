#!/usr/bin/env python3
# Python competitor: unit-5 lang/text benchmarks (contract: u5-competitor-spec.md).
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

CP_BASE = "Hello, wörld! Καλημέρα κόσμε, こんにちは世界 \U0001F680✨ "
SCAN_BASE = "the quick brown fox jumps over the lazy dog\n"
SPLIT_BASE = "alpha,beta,gamma,delta,epsilon,zeta,eta,theta,iota,kappa,"
PIECE = "0123456789abcdef"


def main():
    assert len(CP_BASE.encode("utf-8")) == 74 and len(CP_BASE) == 41

    if selected("string-codepoint-walk"):
        reps = sc(14170)
        raw = (CP_BASE * reps).encode("utf-8")
        text = raw.decode("utf-8")  # decoded once in setup
        exp = (41 * reps, sum(map(ord, CP_BASE)) * reps)

        def walk():
            c = 0
            t = 0
            for ch in text:
                c += 1
                t += ord(ch)
            return (c, t)
        run("string-codepoint-walk", len(raw), "str for + ord", PYVER,
            "UTF-8 decoded to str once in setup", "", walk, lambda r: r == exp)

    if selected("string-byteat-scan"):
        reps = sc(23832)
        data = (SCAN_BASE * reps).encode("utf-8")

        def scan():
            c = 0
            for b in data:
                if b == 10:
                    c += 1
            return c
        run("string-byteat-scan", len(data), "bytes iterate", PYVER, "", "", scan, lambda c: c == reps)

    if selected("string-split"):
        reps = sc(100)
        text = SPLIT_BASE * 18397 + "end"
        tb = len(text.encode("utf-8"))

        def split():
            t = 0
            for _ in range(reps):
                t += len(text.split(","))
            return t
        run("string-split", reps * tb, "str.split", PYVER, "", "", split, lambda t: t == reps * 183971)

    if selected("string-compare"):
        prefix = "".join(chr(c) for c in range(ord("a"), ord("z") + 1)) + "0123"
        a = [prefix + f"{k:02d}" for k in range(64)]
        b = [(s + ".")[:-1] if k % 2 == 0 else s[:-1] + "x" for k, s in enumerate(a)]
        assert all(len(s) == 32 for s in a) and not any(x is y for x, y in zip(a, b))
        n = sc(10000000)

        def compare():
            eq = 0
            for i in range(n):
                if a[i & 63] == b[i & 63]:
                    eq += 1
            return eq
        run("string-compare", n, "str ==", PYVER, "distinct objects; content compare", "", compare,
            lambda e: e == (n + 1) // 2)

    n = sc(1000000)

    def append():
        parts = []
        for _ in range(n):
            parts.append(PIECE)
        return len("".join(parts))
    run("stringbuilder-append", n, "list + join", PYVER, "no builder type: list append then join", "",
        append, lambda L: L == 16 * n)

    def mixed():
        parts = []
        for i in range(n):
            m = i % 3
            if m == 0:
                parts.append(str(i))
            elif m == 1:
                parts.append(str((i % 1000) + 0.5))
            else:
                parts.append("abc")
        return len("".join(parts))
    # expected length by formula: decimal digits of i; float (i%1000)+0.5 is digits(i%1000)+2
    exp_mixed = sum(len(str(i)) if i % 3 == 0 else (len(str(i % 1000)) + 2 if i % 3 == 1 else 3)
                    for i in range(n)) if DIV > 1 else 4592962
    run("stringbuilder-append-mixed", n, "str() + join", PYVER, "repr shortest round-trip floats", "",
        mixed, lambda L: L == exp_mixed)

    if selected("stringbuilder-tostring"):
        pieces = [PIECE] * 1048576
        reps = sc(100)

        def tostring():
            t = 0
            for _ in range(reps):
                t += len("".join(pieces))
            return t
        run("stringbuilder-tostring", reps, "''.join", PYVER, "no builder type: join of the piece list", "",
            tostring, lambda t: t == reps * 16777216)


if __name__ == "__main__":
    main()
