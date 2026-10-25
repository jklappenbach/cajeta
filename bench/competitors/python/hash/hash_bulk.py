#!/usr/bin/env python3
# Python hash competitors — bulk throughput over a deterministic 1 MiB buffer
# (buf[i] = i & 0xFF), the SMHasher bulk metric. One schema-conformant CSV row
# per algorithm: sha256 + md5 via hashlib (stdlib), xxhash3 via the xxhash
# module (skip row if not importable). Cross-check: the digest matches the
# cross-language reference.
import hashlib
import os
import resource
import sys
import time
import tracemalloc

N = 1048576
SHA256_REF = "fbbab289f7f94b25736c58be46a994c441fd02552cc6022352e3d86d2fab7c83"
MD5_REF = "c35cc7d8d91728a0cb052831bc4ef372"
BLAKE3_REF = "64479cf7293960210547db8d982359e0c4ce054525ed7086cf93030828fc0533"
XXH3_REF = 0xD36C0E13A3DF139E


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


def row(run_id, ts, bench, lib, ver, warmup, trials, st, check_ok, status, sz=N):
    mn, med, mean, p95 = st
    mbps = (sz / med * 1e9 / 1048576.0) if med > 0 else 0.0
    pyver = env("PROFILE_LANG_VERSION", "")
    return (
        f"1,{run_id},{ts},{bench},hash,,{sz},,{sz},python,{pyver},{lib},{ver},-OO,"
        f"{warmup},{trials},{mn},{med},{mean},{p95},{mbps:.1f},MB/s,{peak_rss_kb()},"
        f"-1,{_ALLOC},-1,-1,{status},{str(check_ok).lower()},,"
    )


def skip_row(run_id, ts, bench, lib, reason, sz=N):
    pyver = env("PROFILE_LANG_VERSION", "")
    return (
        f"1,{run_id},{ts},{bench},hash,,{sz},,{sz},python,{pyver},{lib},,,0,0,"
        f"-1,-1,-1,-1,,,-1,-1,-1,-1,-1,skipped,,{reason},"
    )


def bench(buf, warmup, trials, f):
    for _ in range(warmup):
        f(buf)
    samples = []
    for _ in range(trials):
        t0 = time.perf_counter_ns()
        f(buf)
        samples.append(time.perf_counter_ns() - t0)
    global _ALLOC
    _ALLOC = alloc_bytes(lambda: f(buf))
    return samples


def main():
    run_id = env("PROFILE_RUN_ID", "local")
    ts = env("PROFILE_RUN_TS", "")
    warmup = int(env("PROFILE_WARMUP", "3"))
    trials = int(env("PROFILE_TRIALS", "10"))
    buf = bytes(i & 0xFF for i in range(N))
    out = []

    # xxhash3 (optional module)
    try:
        import xxhash
        s = bench(buf, warmup, trials, lambda b: xxhash.xxh3_64_intdigest(b, seed=0))
        ok = xxhash.xxh3_64_intdigest(buf, seed=0) == XXH3_REF
        out.append(row(run_id, ts, "xxhash3", "xxhash", xxhash.VERSION, warmup, trials,
                       stats(s), ok, "ok" if ok else "invalid"))
        # xxhash3_128 (XXH3-128; time + cross-check the low64)
        mask = (1 << 64) - 1
        s = bench(buf, warmup, trials, lambda b: xxhash.xxh3_128_intdigest(b, seed=0) & mask)
        ok = (xxhash.xxh3_128_intdigest(buf, seed=0) & mask) == XXH3_REF
        out.append(row(run_id, ts, "xxhash3_128", "xxhash", xxhash.VERSION, warmup, trials,
                       stats(s), ok, "ok" if ok else "invalid"))
        # xxhash3-256k / xxhash3_128-256k — compute-bound (L2-resident) throughput.
        n256 = 262144
        b256 = buf[:n256]
        s = bench(b256, warmup, trials, lambda b: xxhash.xxh3_64_intdigest(b, seed=0))
        ok = xxhash.xxh3_64_intdigest(b256, seed=0) != 0
        out.append(row(run_id, ts, "xxhash3-256k", "xxhash", xxhash.VERSION, warmup, trials,
                       stats(s), ok, "ok" if ok else "invalid", n256))
        s = bench(b256, warmup, trials, lambda b: xxhash.xxh3_128_intdigest(b, seed=0) & mask)
        ok = (xxhash.xxh3_128_intdigest(b256, seed=0) & mask) != 0
        out.append(row(run_id, ts, "xxhash3_128-256k", "xxhash", xxhash.VERSION, warmup, trials,
                       stats(s), ok, "ok" if ok else "invalid", n256))
    except ImportError:
        out.append(skip_row(run_id, ts, "xxhash3", "xxhash", "xxhash module not importable"))
        out.append(skip_row(run_id, ts, "xxhash3_128", "xxhash", "xxhash module not importable"))
        out.append(skip_row(run_id, ts, "xxhash3-256k", "xxhash", "xxhash module not importable", 262144))
        out.append(skip_row(run_id, ts, "xxhash3_128-256k", "xxhash", "xxhash module not importable", 262144))

    # sha256 (hashlib)
    s = bench(buf, warmup, trials, lambda b: hashlib.sha256(b).digest())
    ok = hashlib.sha256(buf).hexdigest() == SHA256_REF
    out.append(row(run_id, ts, "sha256", "hashlib", sys.version.split()[0], warmup, trials,
                   stats(s), ok, "ok" if ok else "invalid"))

    # md5 (hashlib)
    s = bench(buf, warmup, trials, lambda b: hashlib.md5(b).digest())
    ok = hashlib.md5(buf).hexdigest() == MD5_REF
    out.append(row(run_id, ts, "md5", "hashlib", sys.version.split()[0], warmup, trials,
                   stats(s), ok, "ok" if ok else "invalid"))

    # blake3 (optional module; the pip `blake3` binds the Rust reference crate)
    try:
        import blake3
        s = bench(buf, warmup, trials, lambda b: blake3.blake3(b).digest())
        ok = blake3.blake3(buf).hexdigest() == BLAKE3_REF
        out.append(row(run_id, ts, "blake3", "blake3", getattr(blake3, "__version__", "?"),
                       warmup, trials, stats(s), ok, "ok" if ok else "invalid"))
    except ImportError:
        out.append(skip_row(run_id, ts, "blake3", "blake3", "blake3 module not importable"))

    sys.stdout.write("\n".join(out) + "\n")


if __name__ == "__main__":
    main()
