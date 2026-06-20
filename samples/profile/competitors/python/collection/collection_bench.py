#!/usr/bin/env python3
# Python collection competitors — the same four workloads the Cajeta side runs:
# hashmap-int / hashmap-string (Ankerl insert+find shape via dict), hashset-dedup
# (set), arraylist-append (list). One schema-conformant CSV row per (benchmark,
# library). Cross-checks are the exact closed forms.
import os
import resource
import sys
import time

N_INT, N_STR, N_SET, SET_HALF, N_VEC = 50000, 30000, 100000, 50000, 100000
INT_REF = 2499950000
STR_REF = 449985000


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
        f"1,{run_id},{ts},{bench},collection,,{input_size},,{input_size},python,{pyver},{lib},cpython,-OO,"
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


def hashmap_int():
    m = {}
    for i in range(N_INT):
        m[i] = i * 2
    s = 0
    for j in range(N_INT):
        s += m[j]
    return s


def hashmap_string():
    m = {}
    for i in range(N_STR):
        m["key" + str(i)] = i
    s = 0
    for j in range(N_STR):
        s += m["key" + str(j)]
    return s


def hashset_dedup():
    st = set()
    for i in range(N_SET):
        st.add(i % SET_HALF)
    return len(st)


def arraylist_append():
    v = []
    for i in range(N_VEC):
        v.append(i)
    return len(v)


def main():
    run_id = env("PROFILE_RUN_ID", "local")
    ts = env("PROFILE_RUN_TS", "")
    warmup = int(env("PROFILE_WARMUP", "3"))
    trials = int(env("PROFILE_TRIALS", "10"))
    out = []

    s, ok = bench(warmup, trials, hashmap_int, lambda r: r == INT_REF)
    out.append(emit(run_id, ts, "hashmap-int", "dict", N_INT, warmup, trials, stats(s), ok))

    s, ok = bench(warmup, trials, hashmap_string, lambda r: r == STR_REF)
    out.append(emit(run_id, ts, "hashmap-string", "dict", N_STR, warmup, trials, stats(s), ok))

    s, ok = bench(warmup, trials, hashset_dedup, lambda r: r == SET_HALF)
    out.append(emit(run_id, ts, "hashset-dedup", "set", N_SET, warmup, trials, stats(s), ok))

    s, ok = bench(warmup, trials, arraylist_append, lambda r: r == N_VEC)
    out.append(emit(run_id, ts, "arraylist-append", "list", N_VEC, warmup, trials, stats(s), ok))

    sys.stdout.write("\n".join(out) + "\n")


if __name__ == "__main__":
    main()
