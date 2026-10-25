#!/usr/bin/env python3
# Python hash competitors (benchmark-reference unit 8): the hash/ ids of
# bench/hash/XXHash3.md and SipHash.md, same inputs, folds and checks as
# bench/src/main/cajeta/bench/hash/:
#   xxhash3 / xxhash3_128 / siphash: 1e7 hashes of a 32-byte key whose first 8
#     bytes are i little-endian (byte j = (j*31+7) & 0xFF);
#   xxhash3-256k / xxhash3_128-256k: 1e4 hashes of a 256 KB buffer, same rule.
# Libraries (pinned in requirements.txt): xxhash (python-xxhash, the C
# reference), siphash24 (SipHash-2-4 C extension). The interpreter loop and
# the key update are part of each hash's cost, as they are for every Python
# caller. alloc_bytes is tracemalloc's peak over a 1/100-size pass.
import os
import resource
import struct
import sys
import time
import tracemalloc

import siphash24
import xxhash

M = (1 << 64) - 1
N_KEYS = 10_000_000
N_BIG = 10_000
BIG = 262_144


def env(k, d):
    v = os.environ.get(k)
    return v if v else d


RUN_ID, TS, VER = env("PROFILE_RUN_ID", "local"), env("PROFILE_RUN_TS", ""), env("PROFILE_LANG_VERSION", "")
WARMUP, TRIALS = int(env("PROFILE_WARMUP", "3")), int(env("PROFILE_TRIALS", "10"))


def measure(bench, nhash, size, lib, libver, notes, f, want):
    """f(n) hashes n inputs and returns the folded digest tuple."""
    for _ in range(WARMUP):
        f(nhash)
    s, ok = [], True
    for _ in range(TRIALS):
        t0 = time.perf_counter_ns()
        r = f(nhash)
        s.append(time.perf_counter_ns() - t0)
        ok = ok and r == want
    tracemalloc.start()
    f(max(1, nhash // 100))
    alloc = tracemalloc.get_traced_memory()[1]
    tracemalloc.stop()
    s.sort()
    k = len(s)
    med = s[k // 2]
    nbytes = nhash * size
    mbps = nbytes / med * 1e9 / 1048576 if med > 0 else 0.0
    rss = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
    print(f"1,{RUN_ID},{TS},{bench},hash,,{nbytes},,{nbytes},python,{VER},{lib},{libver},cpython,{WARMUP},{TRIALS},"
          f"{s[0]},{med},{sum(s) // k},{s[min(k - 1, k * 95 // 100)]},{mbps:.1f},MB/s,{rss},-1,{alloc},-1,-1,"
          f"{'ok' if ok else 'invalid'},{str(ok).lower()},{notes},", flush=True)


def buf(n):
    return bytearray(((j * 31 + 7) & 0xFF) for j in range(n))


def run64(b, fn):
    def run(n):
        a = 0
        put = struct.pack_into
        for i in range(n):
            put("<Q", b, 0, i)
            a ^= fn(b)
        return (a, 0)
    return run


def run128(b):
    def run(n):
        lo = hi = 0
        put = struct.pack_into
        d = xxhash.xxh3_128_intdigest
        for i in range(n):
            put("<Q", b, 0, i)
            h = d(b)
            lo ^= h & M
            hi ^= h >> 64
        return (lo, hi)
    return run


def main():
    key, big = buf(32), buf(BIG)
    xv = xxhash.VERSION
    measure("xxhash3", N_KEYS, 32, "xxhash", xv, "xxh3_64_intdigest", run64(key, xxhash.xxh3_64_intdigest),
            (0x15AE4A024A0E82DE, 0))
    measure("xxhash3_128", N_KEYS, 32, "xxhash", xv, "xxh3_128_intdigest", run128(key),
            (0x7F4ADB2D9F387C7F, 0xB318FB3EC50537EF))
    measure("xxhash3-256k", N_BIG, BIG, "xxhash", xv, "xxh3_64_intdigest", run64(big, xxhash.xxh3_64_intdigest),
            (0x4F52C915522F9FE0, 0))
    measure("xxhash3_128-256k", N_BIG, BIG, "xxhash", xv, "xxh3_128_intdigest", run128(big),
            (0x4F52C915522F9FE0, 0x3C843C6DA63E2DC3))
    k16 = bytes(range(16))
    vec_ok = siphash24.siphash24(bytes(range(15)), key=k16).intdigest() & M == 0xA129CA6149BE45E5

    def sip(n):
        a = 0
        put = struct.pack_into
        h = siphash24.siphash24
        for i in range(n):
            put("<Q", key, 0, i)
            a ^= h(key, key=k16).intdigest() & M
        return (a, 0) if vec_ok else (0, 0)
    measure("siphash", N_KEYS, 32, "siphash24", "1.9", "siphash24(key=00..0f).intdigest", sip, (0x1A6999CCA3276432, 0))


if __name__ == "__main__":
    main()
