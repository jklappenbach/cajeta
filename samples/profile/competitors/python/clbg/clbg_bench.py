#!/usr/bin/env python3
# Python CLBG competitors — the same three Computer Language Benchmarks Game
# classics the Cajeta side runs, at the same sizes, verified against the
# canonical checksums: clbg-mandelbrot (800², 254099), clbg-fannkuch-redux
# (N=10, 38 / 73196), clbg-spectral-norm (N=100, ≈1.274224). mandelbrot uses
# numpy (the idiomatic fast-python form; pure CPython would dominate the suite);
# fannkuch + spectral-norm are pure CPython — fannkuch(10) is intentionally the
# interpreter-cost data point.
import math
import os
import resource
import sys
import time

MANDEL_N = 800
MANDEL_REF = 254099
FANN_N = 10
SPECTRAL_N = 100
SPECTRAL_REF = 1.274224


def env(k, d):
    v = os.environ.get(k)
    return v if v else d


def peak_rss_kb():
    return resource.getrusage(resource.RUSAGE_SELF).ru_maxrss


def stats(samples):
    s = sorted(samples)
    n = len(s)
    return s[0], s[n // 2], sum(s) // n, s[min(n - 1, n * 95 // 100)]


def emit(run_id, ts, bench, lib, ver, input_size, warmup, trials, st, ok):
    mn, med, mean, p95 = st
    pyver = env("PROFILE_LANG_VERSION", "")
    status = "ok" if ok else "invalid"
    # clbg has no natural throughput unit — leave throughput/unit empty.
    return (
        f"1,{run_id},{ts},{bench},clbg,,{input_size},,{input_size},python,{pyver},{lib},{ver},-OO,"
        f"{warmup},{trials},{mn},{med},{mean},{p95},,,{peak_rss_kb()},"
        f"-1,-1,-1,-1,{status},{str(ok).lower()},,"
    )


def bench(warmup, trials, f, check):
    for _ in range(warmup):
        f()
    samples = []
    ok = True
    for _ in range(trials):
        t0 = time.perf_counter_ns()
        r = f()
        samples.append(time.perf_counter_ns() - t0)
        ok = check(r)
    return samples, ok


def mandelbrot_numpy(np):
    n = MANDEL_N
    px = np.arange(n, dtype=np.float64)
    cr = 2.0 * px / n - 1.5
    ci = 2.0 * px / n - 1.0
    CR, CI = np.meshgrid(cr, ci)
    zr = np.zeros((n, n)); zi = np.zeros((n, n))
    member = np.ones((n, n), dtype=bool)
    with np.errstate(over="ignore", invalid="ignore"):
        for _ in range(50):
            nzr = zr * zr - zi * zi + CR
            nzi = 2.0 * zr * zi + CI
            zr, zi = nzr, nzi
            member &= ~(zr * zr + zi * zi > 4.0)
    return int(member.sum())


def fannkuch(n):
    perm = list(range(n))
    perm1 = list(range(n))
    count = [0] * n
    max_flips = checksum = perm_count = 0
    r = n
    while True:
        while r != 1:
            count[r - 1] = r
            r -= 1
        perm = perm1[:]
        flips = 0
        k = perm[0]
        while k != 0:
            perm[:k + 1] = perm[:k + 1][::-1]
            flips += 1
            k = perm[0]
        if flips > max_flips:
            max_flips = flips
        checksum += flips if perm_count % 2 == 0 else -flips
        perm_count += 1
        while True:
            if r == n:
                return max_flips, checksum
            p0 = perm1[0]
            for i in range(r):
                perm1[i] = perm1[i + 1]
            perm1[r] = p0
            count[r] -= 1
            if count[r] > 0:
                break
            r += 1


def spectral_norm(n):
    def a(i, j):
        return 1.0 / ((i + j) * (i + j + 1) // 2 + i + 1)

    def mul_av(v):
        return [sum(a(i, j) * v[j] for j in range(n)) for i in range(n)]

    def mul_atv(v):
        return [sum(a(j, i) * v[j] for j in range(n)) for i in range(n)]

    u = [1.0] * n
    v = [0.0] * n
    for _ in range(10):
        t = mul_av(u)
        v = mul_atv(t)
        t = mul_av(v)
        u = mul_atv(t)
    vbv = sum(u[i] * v[i] for i in range(n))
    vv = sum(v[i] * v[i] for i in range(n))
    return math.sqrt(vbv / vv)


def main():
    run_id = env("PROFILE_RUN_ID", "local")
    ts = env("PROFILE_RUN_TS", "")
    warmup = int(env("PROFILE_WARMUP", "3"))
    trials = int(env("PROFILE_TRIALS", "10"))
    out = []

    try:
        import numpy as np
        s, ok = bench(warmup, trials, lambda: mandelbrot_numpy(np), lambda r: r == MANDEL_REF)
        out.append(emit(run_id, ts, "clbg-mandelbrot", "numpy", np.__version__,
                        MANDEL_N * MANDEL_N, warmup, trials, stats(s), ok))
    except ImportError:
        out.append(
            f"1,{run_id},{ts},clbg-mandelbrot,clbg,,{MANDEL_N*MANDEL_N},,{MANDEL_N*MANDEL_N},python,"
            f"{env('PROFILE_LANG_VERSION','')},numpy,,,0,0,-1,-1,-1,-1,,,-1,-1,-1,-1,-1,skipped,,numpy not importable,")

    s, ok = bench(warmup, trials, lambda: fannkuch(FANN_N), lambda r: r == (38, 73196))
    out.append(emit(run_id, ts, "clbg-fannkuch-redux", "cpython", sys.version.split()[0],
                    FANN_N, warmup, trials, stats(s), ok))

    s, ok = bench(warmup, trials, lambda: spectral_norm(SPECTRAL_N), lambda r: abs(r - SPECTRAL_REF) < 1e-3)
    out.append(emit(run_id, ts, "clbg-spectral-norm", "cpython", sys.version.split()[0],
                    SPECTRAL_N, warmup, trials, stats(s), ok))

    sys.stdout.write("\n".join(out) + "\n")


if __name__ == "__main__":
    main()
