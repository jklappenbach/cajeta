#!/usr/bin/env python3
# Python math competitors — the same three f64 kernels the Cajeta side runs, via
# numpy (vectorized + BLAS): saxpy (z = 2.5*x + y over 1M), dot-product
# (np.dot over 1M), matmul (200×200, A identity · B via @, i.e. dgemm). Same init
# ⇒ exact reference checksums. Throughput in GFLOP/s. numpy is the headline
# competitor — it shows what BLAS/SIMD buys over the scalar kernels. A skip row if
# numpy isn't importable.
import os
import resource
import sys
import time

N = 1000000
NDIM = 200
SAXPY_REF = 148250000.0
DOT_REF = 148499899.0
MATMUL_REF = 1980000.0


def env(k, d):
    v = os.environ.get(k)
    return v if v else d


def peak_rss_kb():
    return resource.getrusage(resource.RUSAGE_SELF).ru_maxrss


def stats(samples):
    s = sorted(samples)
    n = len(s)
    return s[0], s[n // 2], sum(s) // n, s[min(n - 1, n * 95 // 100)]


def emit(run_id, ts, bench, lib, ver, input_size, flops, warmup, trials, st, ok):
    mn, med, mean, p95 = st
    gflops = (flops / med) if med > 0 else 0.0  # FLOP per ns == GFLOP/s
    pyver = env("PROFILE_LANG_VERSION", "")
    status = "ok" if ok else "invalid"
    return (
        f"1,{run_id},{ts},{bench},math,,{input_size},,{input_size},python,{pyver},{lib},{ver},-OO,"
        f"{warmup},{trials},{mn},{med},{mean},{p95},{gflops:.3f},GFLOP/s,{peak_rss_kb()},"
        f"-1,-1,-1,-1,{status},{str(ok).lower()},,"
    )


def skip_row(run_id, ts, bench, input_size, reason):
    pyver = env("PROFILE_LANG_VERSION", "")
    return (
        f"1,{run_id},{ts},{bench},math,,{input_size},,{input_size},python,{pyver},numpy,,,0,0,"
        f"-1,-1,-1,-1,,,-1,-1,-1,-1,-1,skipped,,{reason},"
    )


def approx(a, b):
    return abs(a - b) <= 1e-6 * max(abs(b), 1.0)


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


def main():
    run_id = env("PROFILE_RUN_ID", "local")
    ts = env("PROFILE_RUN_TS", "")
    warmup = int(env("PROFILE_WARMUP", "3"))
    trials = int(env("PROFILE_TRIALS", "10"))

    try:
        import numpy as np
    except ImportError:
        sys.stdout.write("\n".join(
            skip_row(run_id, ts, b, n, "numpy not importable")
            for b, n in (("saxpy", N), ("dot-product", N), ("matmul", NDIM * NDIM))) + "\n")
        return

    ver = np.__version__
    i = np.arange(N, dtype=np.float64)
    x = i % 100
    ys = i % 50
    yd = i % 7

    # saxpy: z = 2.5*x + y ; checksum = sum
    s, ok = bench(warmup, trials, lambda: float((2.5 * x + ys).sum()), lambda r: approx(r, SAXPY_REF))
    print(emit(run_id, ts, "saxpy", "numpy", ver, N, 2.0 * N, warmup, trials, stats(s), ok))

    # dot-product: np.dot
    s, ok = bench(warmup, trials, lambda: float(np.dot(x, yd)), lambda r: approx(r, DOT_REF))
    print(emit(run_id, ts, "dot-product", "numpy", ver, N, 2.0 * N, warmup, trials, stats(s), ok))

    # matmul: 200x200 via @ (dgemm)
    A = np.zeros((NDIM, NDIM), dtype=np.float64)
    np.fill_diagonal(A, 1.0)
    B = (np.arange(NDIM * NDIM, dtype=np.float64) % 100).reshape(NDIM, NDIM)
    s, ok = bench(warmup, trials, lambda: float((A @ B).sum()), lambda r: approx(r, MATMUL_REF))
    print(emit(run_id, ts, "matmul", "numpy", ver, NDIM * NDIM, 2.0 * NDIM ** 3, warmup, trials, stats(s), ok))


if __name__ == "__main__":
    main()
