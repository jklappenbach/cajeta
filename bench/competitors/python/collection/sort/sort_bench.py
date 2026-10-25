#!/usr/bin/env python3
# Python sort competitors — sort 50K int across the same four input patterns the
# Cajeta side uses (random / ascending / descending / dups), plus a 50K float
# random sort. One schema-conformant CSV row per (benchmark, variant, library).
# Same splitmix64 input sequence as every other language. Cross-check: output
# non-decreasing and (for int) the sum is preserved. Library: list.sort()
# (CPython's Timsort — adaptive + stable; the only sort in the stdlib).
import os
import resource
import sys
import time
import tracemalloc

N = 50000
SEED = 0x123456789ABCDEF0
MASK = (1 << 64) - 1


def env(k, d):
    v = os.environ.get(k)
    return v if v else d


def peak_rss_kb():
    return resource.getrusage(resource.RUSAGE_SELF).ru_maxrss


_ALLOC = -1


def alloc_bytes(run):
    """Peak Python-heap bytes allocated by one execution (tracemalloc). Note: C-
    extension allocations outside CPython (e.g. numpy buffers) are not traced."""
    tracemalloc.start()
    tracemalloc.reset_peak()
    run()
    _peak = tracemalloc.get_traced_memory()[1]
    tracemalloc.stop()
    return _peak


class Split:
    def __init__(self, s):
        self.s = s

    def next(self):
        self.s = (self.s + 0x9E3779B97F4A7C15) & MASK
        z = self.s
        z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & MASK
        z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & MASK
        return z ^ (z >> 31)


def signed64(u):
    return u - (1 << 64) if u >= (1 << 63) else u


def make_i64(variant):
    g = Split(SEED)
    if variant == "random":
        return [signed64(g.next()) for _ in range(N)]
    if variant == "ascending":
        return list(range(N))
    if variant == "descending":
        return list(range(N - 1, -1, -1))
    return [i % 16 for i in range(N)]  # dups


def make_f64():
    g = Split(SEED)
    return [(g.next() % 100000000) / 7.0 for _ in range(N)]


def is_sorted(w):
    return all(w[i] <= w[i + 1] for i in range(len(w) - 1))


def stats(samples):
    s = sorted(samples)
    n = len(s)
    return s[0], s[n // 2], sum(s) // n, s[min(n - 1, n * 95 // 100)]


def emit(run_id, ts, bench, variant, lib, warmup, trials, st, ok):
    mn, med, mean, p95 = st
    mels = (N / med * 1e9 / 1e6) if med > 0 else 0.0
    pyver = env("PROFILE_LANG_VERSION", "")
    status = "ok" if ok else "invalid"
    return (
        f"1,{run_id},{ts},{bench},sort,,{N},,{N},python,{pyver},{lib},cpython,-OO,"
        f"{warmup},{trials},{mn},{med},{mean},{p95},{mels:.2f},Melem/s,{peak_rss_kb()},"
        f"-1,{_ALLOC},-1,-1,{status},{str(ok).lower()},,{variant}"
    )


def bench(input_list, warmup, trials, check_sum):
    want = sum(input_list) if check_sum else None
    for _ in range(warmup):
        w = list(input_list)
        w.sort()
    samples = []
    ok = True
    for _ in range(trials):
        w = list(input_list)
        t0 = time.perf_counter_ns()
        w.sort()
        samples.append(time.perf_counter_ns() - t0)
        ok = is_sorted(w) and (not check_sum or sum(w) == want)
    global _ALLOC
    _ALLOC = alloc_bytes(lambda: list(input_list).sort())
    return samples, ok


def main():
    run_id = env("PROFILE_RUN_ID", "local")
    ts = env("PROFILE_RUN_TS", "")
    warmup = int(env("PROFILE_WARMUP", "3"))
    trials = int(env("PROFILE_TRIALS", "10"))
    out = []

    for variant in ("random", "ascending", "descending", "dups"):
        s, ok = bench(make_i64(variant), warmup, trials, True)
        out.append(emit(run_id, ts, "sort-int64", variant, "Timsort", warmup, trials, stats(s), ok))
    # stable == Timsort (random only)
    s, ok = bench(make_i64("random"), warmup, trials, True)
    out.append(emit(run_id, ts, "sort-stable-int64", "", "Timsort", warmup, trials, stats(s), ok))
    # f64 random
    s, ok = bench(make_f64(), warmup, trials, False)
    out.append(emit(run_id, ts, "sort-f64", "", "Timsort", warmup, trials, stats(s), ok))

    sys.stdout.write("\n".join(out) + "\n")


if __name__ == "__main__":
    main()
