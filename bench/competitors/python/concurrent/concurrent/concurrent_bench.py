#!/usr/bin/env python3
# Python concurrency competitors for bench/concurrent/*.md — the ids whose page
# lists Python (threading, asyncio, contextvars), the same workload, size and
# check as the Cajeta side; the other ids are skipped rows with the reason.
# CPython 3.12 has a GIL: the threaded rows (atomic-contended-16,
# mutex-contended-*) are serialised by it, and the notes say so. CPython has no
# lock-free atomic integer; itertools.count's __next__ is atomic under the GIL
# and is the idiomatic counter, so it stands in for fetchAdd. Under
# CAJETA_BENCH_SMOKE=1 the threaded rows run 1e4 operations per thread, the 1e8
# rows 1e6 and the sleep row 100 sleeps; those rows say "smoke size" in notes.
# The 1e8 rows run at most 1 warm-up and 3 trials (CPython has no JIT to warm).
import asyncio
import contextvars
import itertools
import os
import resource
import sys
import threading
import time
import tracemalloc

def env(k, d):
    v = os.environ.get(k)
    return v if v else d

RUN_ID = env("PROFILE_RUN_ID", "local")
TS = env("PROFILE_RUN_TS", "")
WARMUP = int(env("PROFILE_WARMUP", "3"))
TRIALS = int(env("PROFILE_TRIALS", "10"))
SMOKE = env("CAJETA_BENCH_SMOKE", "") in ("1", "true")
VER = env("PROFILE_LANG_VERSION", "")


def peak_rss_kb():
    return resource.getrusage(resource.RUSAGE_SELF).ru_maxrss


def smoke_note(what):
    return ("smoke size: " + what) if SMOKE else ""


def join(a, b):
    return a if not b else a + "; " + b


def emit(bench, lib, input_size, warm, samples, ok, alloc, notes):
    s = sorted(samples)
    n = len(s)
    mn, med, mean, p95 = s[0], s[n // 2], sum(s) // n, s[min(n - 1, n * 95 // 100)]
    mops = (input_size / med * 1e9 / 1e6) if med > 0 else 0.0
    status = "ok" if ok else "invalid"
    print(f"1,{RUN_ID},{TS},{bench},concurrent,,{input_size},,{input_size},python,{VER},{lib},cpython,-OO,"
          f"{warm},{n},{mn},{med},{mean},{p95},{mops:.3f},Mop/s,{peak_rss_kb()},"
          f"-1,{alloc},-1,-1,{status},{str(ok).lower()},{notes},", flush=True)


def skip(bench, lib, input_size, reason):
    print(f"1,{RUN_ID},{TS},{bench},concurrent,,{input_size},,{input_size},python,{VER},{lib},,,0,0,"
          f"-1,-1,-1,-1,,,-1,-1,-1,-1,-1,skipped,,{reason},", flush=True)


def bench(bid, lib, input_size, notes, f, want, big=False):
    warm, trials = (min(WARMUP, 1), min(TRIALS, 3)) if big else (WARMUP, TRIALS)
    ok = True
    for _ in range(warm):
        f()
    samples = []
    for _ in range(trials):
        t0 = time.perf_counter_ns()
        try:
            r = f()
        except Exception:  # a failing workload is an invalid row, not a crash
            r = None
        samples.append(time.perf_counter_ns() - t0)
        ok = ok and r == want
    alloc = -1
    if not big:  # tracemalloc over a 1e8 loop would take minutes; -1 = not traced
        tracemalloc.start()
        f()
        alloc = tracemalloc.get_traced_memory()[1]
        tracemalloc.stop()
    emit(bid, lib, input_size, warm, samples, ok, alloc, notes)


def threads(t, body):
    out = [None] * t
    def run(k):
        out[k] = body(k)
    ts = [threading.Thread(target=run, args=(k,)) for k in range(t)]
    for th in ts:
        th.start()
    for th in ts:
        th.join()
    return out


N1 = 1_000_000 if SMOKE else 100_000_000
PER = 10_000 if SMOKE else 1_000_000
NSP = 10_000 if SMOKE else 100_000
NSLEEP = 100 if SMOKE else 10_000
NCH = 10_000 if SMOKE else 5_000_000
S1 = smoke_note("1e6 ops")
SC = smoke_note("1e4 ops per thread")
GIL = "GIL: CPython 3.12 runs one thread at a time"


def counter_n():
    c = itertools.count()
    nx = c.__next__
    for _ in range(N1):
        nx()
    return nx()


def counter_contended():
    c = itertools.count()
    def body(_):
        nx = c.__next__
        for _ in range(PER):
            nx()
        return 0
    threads(16, body)
    return next(c)


def mutex_n():
    lock = threading.Lock()
    v = 0
    for _ in range(N1):
        with lock:
            v += 1
    return v


def mutex_contended(t):
    lock = threading.Lock()
    box = [0]
    def body(_):
        for _ in range(PER):
            with lock:
                box[0] += 1
        return 0
    threads(t, body)
    return box[0]


FL = contextvars.ContextVar("fl", default=0)


def fiberlocal_n():
    s = 0
    st, gt = FL.set, FL.get
    for i in range(N1):
        st(i)
        s += gt()
    return s


async def leaf(i):
    return i + 1


async def spawn_await_loop():
    total = 0
    for i in range(NSP):
        total += await asyncio.create_task(leaf(i))
    return total


async def sleeps(warm):
    out = []
    for k in range(warm + NSLEEP):
        t0 = time.perf_counter_ns()
        await asyncio.sleep(0.001)
        over = time.perf_counter_ns() - t0 - 1_000_000
        if k >= warm:
            out.append(max(0, over))
    return out


def main():
    bench("atomic-fetchadd", "itertools.count", N1,
          join("no lock-free atomic in CPython: itertools.count.__next__ is atomic under the GIL", S1),
          counter_n, N1, big=True)
    skip("atomic-fetchadd-32", "", N1, "AtomicInt32.md lists no Python competitor (CPython ints are unbounded)")
    skip("atomic-cas", "", N1, "no compare-and-swap in the CPython stdlib")
    bench("atomic-contended-16", "itertools.count+threading", 16 * PER,
          join("16 threading.Thread; " + GIL + " so the GIL decides the number", SC), counter_contended, 16 * PER)
    bench("mutex-uncontended", "threading.Lock", N1, join("with lock: v += 1 on one thread", S1), mutex_n, N1, big=True)
    for t in (4, 16):
        bench(f"mutex-contended-{t}", "threading.Lock+threading", t * PER,
              join(f"{t} threading.Thread; {GIL} so the GIL decides the number", SC),
              lambda t=t: mutex_contended(t), t * PER)
    skip("rwlock-read-heavy", "", 16 * PER, "no reader-writer lock in the CPython stdlib (RwLock.md lists no Python)")
    skip("lock-uncontended", "", N1, "Lock.md lists no Python competitor (threading.Lock is the mutex-uncontended row)")
    skip("semaphore-acquire-release", "", 16 * PER, "Semaphore.md lists no Python competitor")
    for c in ("channel-spsc", "channel-mpsc", "channel-mpmc"):
        skip(c, "", NCH, "Channel.md lists no Python competitor")

    warm = 10
    samples = asyncio.run(sleeps(warm))
    emit("fiber-sleep-wake", "asyncio.sleep", 1, warm, samples, len(samples) == NSLEEP, -1,
         join("row = overshoot past 1 ms per sleep over the trials; one asyncio loop", smoke_note("100 sleeps")))

    bench("fiberlocal-get-set", "contextvars", N1, join("ContextVar.set(i) then get() per pass", S1),
          fiberlocal_n, N1 * (N1 - 1) // 2, big=True)
    bench("task-spawn-await", "asyncio", NSP, join("create_task then await each", smoke_note("1e4 tasks")),
          lambda: asyncio.run(spawn_await_loop()), NSP * (NSP + 1) // 2)


if __name__ == "__main__":
    main()
