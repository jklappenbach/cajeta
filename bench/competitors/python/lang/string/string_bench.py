#!/usr/bin/env python3
# Python string competitors — the same four workloads the Cajeta side runs over a
# ~360 KB ASCII text (base "the quick brown fox jumps over the lazy dog " × 8192):
# string-search (absent needle), string-replace (fox→feline), string-uppercase,
# string-build-concat (build 4000 chars). One schema-conformant CSV row per
# (benchmark, library). Stdlib str idioms (find / replace / upper / "".join).
import os
import resource
import sys
import time
import tracemalloc

BASE = "the quick brown fox jumps over the lazy dog "
REPEAT = 8192
PIECE = "abcdefgh"
BUILD_ITERS = 500


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


def stats(samples):
    s = sorted(samples)
    n = len(s)
    return s[0], s[n // 2], sum(s) // n, s[min(n - 1, n * 95 // 100)]


def emit(run_id, ts, bench, lib, nbytes, warmup, trials, st, ok):
    mn, med, mean, p95 = st
    mbps = (nbytes / med * 1e9 / 1048576.0) if med > 0 else 0.0
    pyver = env("PROFILE_LANG_VERSION", "")
    status = "ok" if ok else "invalid"
    return (
        f"1,{run_id},{ts},{bench},string,,{nbytes},,{nbytes},python,{pyver},{lib},cpython,-OO,"
        f"{warmup},{trials},{mn},{med},{mean},{p95},{mbps:.1f},MB/s,{peak_rss_kb()},"
        f"-1,{_ALLOC},-1,-1,{status},{str(ok).lower()},,"
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
    global _ALLOC
    _ALLOC = alloc_bytes(f)
    return samples, ok


def main():
    run_id = env("PROFILE_RUN_ID", "local")
    ts = env("PROFILE_RUN_TS", "")
    warmup = int(env("PROFILE_WARMUP", "3"))
    trials = int(env("PROFILE_TRIALS", "10"))

    text = BASE * REPEAT
    nbytes = len(text)
    needle = "zzzzqx-not-present"
    out = []

    s, ok = bench(warmup, trials, lambda: text.find(needle), lambda r: r == -1)
    out.append(emit(run_id, ts, "string-search", "str.find", nbytes, warmup, trials, stats(s), ok))

    s, ok = bench(warmup, trials, lambda: text.replace("fox", "feline"), lambda r: len(r) > nbytes)
    out.append(emit(run_id, ts, "string-replace", "str.replace", nbytes, warmup, trials, stats(s), ok))

    s, ok = bench(warmup, trials, lambda: text.upper(), lambda r: len(r) == nbytes)
    out.append(emit(run_id, ts, "string-uppercase", "str.upper", nbytes, warmup, trials, stats(s), ok))

    s, ok = bench(warmup, trials, lambda: "".join(PIECE for _ in range(BUILD_ITERS)),
                  lambda r: len(r) == BUILD_ITERS * 8)
    out.append(emit(run_id, ts, "string-build-concat", "str.join", BUILD_ITERS * 8,
                    warmup, trials, stats(s), ok))

    sys.stdout.write("\n".join(out) + "\n")


if __name__ == "__main__":
    main()
