#!/usr/bin/env python3
# Python hasher competitors (benchmark-reference unit 8) for
# bench/hash/DefaultHasher.md, same inputs and folds as
# bench/src/main/cajeta/bench/hash/, with the builtin hash() a dict calls:
#   defaulthasher-string: 1e7 hash() cycling 1024 keys "k-" + (1e13 + k).
#     CPython caches a str's hash in the object, so after the first pass this
#     is the cached value plus the call; that is what a dict pays for a key
#     object it has seen, and it is noted on the row;
#   defaulthasher-int: 1e8 hash() of 0 .. 1e8-1 (an int below 2**61-1 hashes
#     to itself).
# Fold: acc ^= h; low += h & 0xFFFF (h as unsigned 64). The str hash is
# randomized per process (PYTHONHASHSEED), so the check is a second pass
# reproducing (acc, low) and neighbours hashing apart. Python is not on
# bench/hash/Hash.md, so there is no hash-struct row.
import os
import resource
import sys
import time
import tracemalloc

M = (1 << 64) - 1


def env(k, d):
    v = os.environ.get(k)
    return v if v else d


RUN_ID, TS, VER = env("PROFILE_RUN_ID", "local"), env("PROFILE_RUN_TS", ""), env("PROFILE_LANG_VERSION", "")
WARMUP, TRIALS = int(env("PROFILE_WARMUP", "3")), int(env("PROFILE_TRIALS", "10"))
PY_VER = sys.version.split()[0]


def measure(bench, n, notes, f, extra_ok):
    first = f(n)
    for _ in range(WARMUP):
        f(n)
    s, ok = [], extra_ok and first[1] > 0
    for _ in range(TRIALS):
        t0 = time.perf_counter_ns()
        r = f(n)
        s.append(time.perf_counter_ns() - t0)
        ok = ok and r == first
    tracemalloc.start()
    f(max(1, n // 100))
    alloc = tracemalloc.get_traced_memory()[1]
    tracemalloc.stop()
    s.sort()
    k = len(s)
    med = s[k // 2]
    mops = n / med * 1e3 if med > 0 else 0.0
    rss = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
    print(f"1,{RUN_ID},{TS},{bench},hash,,{n},,{n},python,{VER},builtins,{PY_VER},cpython,{WARMUP},{TRIALS},"
          f"{s[0]},{med},{sum(s) // k},{s[min(k - 1, k * 95 // 100)]},{mops:.2f},Mop/s,{rss},-1,{alloc},-1,-1,"
          f"{'ok' if ok else 'invalid'},{str(ok).lower()},{notes},", flush=True)


def main():
    pool = [f"k-{10_000_000_000_000 + k}" for k in range(1024)]
    s_ok = len(pool[0]) == 16 and hash(pool[0]) != hash(pool[1])

    def strs(n):
        a = l = 0
        for i in range(n):
            h = hash(pool[i & 1023]) & M
            a ^= h
            l += h & 0xFFFF
        return (a, l)

    def ints(n):
        a = l = 0
        for i in range(n):
            h = hash(i) & M
            a ^= h
            l += h & 0xFFFF
        return (a, l)

    measure("defaulthasher-string", 10_000_000, "hash(str); CPython caches a str's hash in the object", strs, s_ok)
    measure("defaulthasher-int", 100_000_000, "hash(int): identity below 2**61-1", ints, hash(0) != hash(1))


if __name__ == "__main__":
    main()
