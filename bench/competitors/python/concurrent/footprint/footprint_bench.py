#!/usr/bin/env python3
# concurrent/tasks-1m-footprint, Python asyncio (Tasks.md): park 1e6 asyncio
# tasks on one asyncio.Queue, read the resident set while all are parked, then
# release them with one token each and await them all. Own process, so
# peak_rss_kb (the row's memory metric) is this workload's. One trial: the row
# is memory; the time is spawn + park + release. Skips under 16 GB MemAvailable.
# CAJETA_BENCH_SMOKE=1 parks 1e4 tasks and says so in notes.
import asyncio
import os
import resource
import time


def env(k, d):
    v = os.environ.get(k)
    return v if v else d


def kb(path, key):
    try:
        with open(path) as f:
            for line in f:
                if line.startswith(key):
                    return int(line.split()[1])
    except OSError:
        pass
    return -1


SMOKE = env("CAJETA_BENCH_SMOKE", "") in ("1", "true")
N = 10_000 if SMOKE else 1_000_000
RUN_ID, TS, VER = env("PROFILE_RUN_ID", "local"), env("PROFILE_RUN_TS", ""), env("PROFILE_LANG_VERSION", "")
HEAD = f"1,{RUN_ID},{TS},tasks-1m-footprint,concurrent,,{N},,{N},python,{VER},asyncio"


async def park(q, started, done):
    started[0] += 1
    await q.get()
    done[0] += 1


async def run():
    q = asyncio.Queue()
    started, done = [0], [0]
    before = kb("/proc/self/status", "VmRSS:")
    tasks = [asyncio.create_task(park(q, started, done)) for _ in range(N)]
    while started[0] < N:
        await asyncio.sleep(0)
    await asyncio.sleep(0)  # every task is now inside q.get()
    parked_rss = kb("/proc/self/status", "VmRSS:")
    parked = started[0]
    for _ in range(N):
        q.put_nowait(1)
    await asyncio.gather(*tasks)
    return parked, done[0], before, parked_rss


def main():
    avail = kb("/proc/meminfo", "MemAvailable:")
    if 0 <= avail < 16 * 1024 * 1024:
        print(f"{HEAD},,,0,0,-1,-1,-1,-1,,,-1,-1,-1,-1,-1,skipped,,MemAvailable {avail // 1024} MiB is under the 16 GB this row needs,")
        return
    t0 = time.perf_counter_ns()
    parked, done, before, parked_rss = asyncio.run(run())
    ns = time.perf_counter_ns() - t0
    ok = parked == N and done == N
    peak = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
    grew = parked_rss - before
    notes = f"one asyncio.Queue; {N} tasks parked in get(); RSS grew {grew} KiB while parked = {grew * 1024 // N} B/task; one trial"
    if SMOKE:
        notes += "; smoke size: 1e4 tasks"
    print(f"{HEAD},cpython,-OO,0,1,{ns},{ns},{ns},{ns},{N / ns * 1e3:.3f},Mop/s,{peak},-1,-1,-1,{grew},"
          f"{'ok' if ok else 'invalid'},{str(ok).lower()},{notes},")


if __name__ == "__main__":
    main()
