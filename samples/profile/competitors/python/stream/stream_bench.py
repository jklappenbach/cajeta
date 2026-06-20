#!/usr/bin/env python3
# Python stream competitors — the same two workloads the Cajeta side runs over
# xs[i] = i % 1000 (N=1M): stream-filter-map-reduce (a generator-expression sum)
# and stream-parallel-reduce (plain sum — the GIL makes a thread pool no faster
# for CPU-bound int work, so this is honestly the sequential idiom). One schema-
# conformant CSV row per benchmark. Cross-check = the exact sums (250000000 /
# 499500000).
import os
import resource
import sys
import time

N = 1000000
FMR_REF = 250000000
PR_REF = 499500000


def env(k, d):
    v = os.environ.get(k)
    return v if v else d


def peak_rss_kb():
    return resource.getrusage(resource.RUSAGE_SELF).ru_maxrss


def stats(samples):
    s = sorted(samples)
    n = len(s)
    return s[0], s[n // 2], sum(s) // n, s[min(n - 1, n * 95 // 100)]


def emit(run_id, ts, bench, lib, input_size, warmup, trials, st, ok):
    mn, med, mean, p95 = st
    mops = (input_size / med * 1e9 / 1e6) if med > 0 else 0.0
    pyver = env("PROFILE_LANG_VERSION", "")
    status = "ok" if ok else "invalid"
    return (
        f"1,{run_id},{ts},{bench},stream,,{input_size},,{input_size},python,{pyver},{lib},cpython,-OO,"
        f"{warmup},{trials},{mn},{med},{mean},{p95},{mops:.2f},Mop/s,{peak_rss_kb()},"
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


def main():
    run_id = env("PROFILE_RUN_ID", "local")
    ts = env("PROFILE_RUN_TS", "")
    warmup = int(env("PROFILE_WARMUP", "3"))
    trials = int(env("PROFILE_TRIALS", "10"))
    xs = [i % 1000 for i in range(N)]
    out = []

    s, ok = bench(warmup, trials, lambda: sum(x + 1 for x in xs if x % 2 == 0), lambda r: r == FMR_REF)
    out.append(emit(run_id, ts, "stream-filter-map-reduce", "genexpr", N, warmup, trials, stats(s), ok))

    s, ok = bench(warmup, trials, lambda: sum(xs), lambda r: r == PR_REF)
    out.append(emit(run_id, ts, "stream-parallel-reduce", "sum (GIL)", N, warmup, trials, stats(s), ok))

    sys.stdout.write("\n".join(out) + "\n")


if __name__ == "__main__":
    main()
