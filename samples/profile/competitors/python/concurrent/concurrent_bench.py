#!/usr/bin/env python3
# Python concurrency competitors — the two workloads the Cajeta side runs:
# atomic-fetchadd (recorded as a `skip` — CPython has no lock-free atomic integer
# in the stdlib; a GIL-protected `+= 1` is not the same primitive) and
# task-spawn-await (20K: spawn an asyncio task computing i+1, await it, accumulate
# → sum == N*(N+1)/2 — asyncio is the lightweight-task model, contrast with OS
# threads and goroutines). One schema-conformant CSV row per benchmark.
import asyncio
import os
import resource
import sys
import time
import tracemalloc

N_ATOMIC = 1000000
N_SPAWN = 20000
SPAWN_REF = 200010000


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


def emit(run_id, ts, bench, lib, input_size, warmup, trials, st, ok):
    mn, med, mean, p95 = st
    mops = (input_size / med * 1e9 / 1e6) if med > 0 else 0.0
    pyver = env("PROFILE_LANG_VERSION", "")
    status = "ok" if ok else "invalid"
    return (
        f"1,{run_id},{ts},{bench},concurrent,,{input_size},,{input_size},python,{pyver},{lib},cpython,-OO,"
        f"{warmup},{trials},{mn},{med},{mean},{p95},{mops:.3f},Mop/s,{peak_rss_kb()},"
        f"-1,{_ALLOC},-1,-1,{status},{str(ok).lower()},,"
    )


def skip_row(run_id, ts, bench, input_size, reason):
    pyver = env("PROFILE_LANG_VERSION", "")
    return (
        f"1,{run_id},{ts},{bench},concurrent,,{input_size},,{input_size},python,{pyver},atomic,,,0,0,"
        f"-1,-1,-1,-1,,,-1,-1,-1,-1,-1,skipped,,{reason},"
    )


async def leaf(i):
    return i + 1


async def spawn_await_loop():
    total = 0
    for i in range(N_SPAWN):
        total += await asyncio.create_task(leaf(i))
    return total


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
    out = []

    out.append(skip_row(run_id, ts, "atomic-fetchadd", N_ATOMIC,
                        "no lock-free atomic in the CPython stdlib (GIL-protected += is not the same primitive)"))

    s, ok = bench(warmup, trials, lambda: asyncio.run(spawn_await_loop()), lambda r: r == SPAWN_REF)
    out.append(emit(run_id, ts, "task-spawn-await", "asyncio", N_SPAWN, warmup, trials, stats(s), ok))

    sys.stdout.write("\n".join(out) + "\n")


if __name__ == "__main__":
    main()
