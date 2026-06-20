#!/usr/bin/env python3
# Python time competitors — the same two workloads the Cajeta side runs:
# time-instant-arith (1M) and time-localdate-arith (100K), using stdlib datetime.
# One schema-conformant CSV row per benchmark. Cross-check = the exact closed-form
# sum (500006500000 / 5000050000).
import os
import resource
import sys
import time
from datetime import date, datetime, timedelta, timezone

N_INSTANT = 1000000
N_DATE = 100000
INSTANT_REF = 500006500000
DATE_REF = 5000050000
EPOCH_ORDINAL = date(1970, 1, 1).toordinal()


def env(k, d):
    v = os.environ.get(k)
    return v if v else d


def peak_rss_kb():
    return resource.getrusage(resource.RUSAGE_SELF).ru_maxrss


def stats(samples):
    s = sorted(samples)
    n = len(s)
    return s[0], s[n // 2], sum(s) // n, s[min(n - 1, n * 95 // 100)]


def emit(run_id, ts, bench, input_size, warmup, trials, st, ok):
    mn, med, mean, p95 = st
    mops = (input_size / med * 1e9 / 1e6) if med > 0 else 0.0
    pyver = env("PROFILE_LANG_VERSION", "")
    status = "ok" if ok else "invalid"
    return (
        f"1,{run_id},{ts},{bench},time,,{input_size},,{input_size},python,{pyver},datetime,cpython,-OO,"
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


def instant_arith():
    utc = timezone.utc
    seven = timedelta(seconds=7)
    s = 0
    for i in range(N_INSTANT):
        t = datetime.fromtimestamp(i, utc)
        t2 = t + seven
        s += int(t2.timestamp())
    return s


def localdate_arith():
    one = timedelta(days=1)
    s = 0
    for i in range(N_DATE):
        d = date.fromordinal(EPOCH_ORDINAL + i)
        d2 = d + one
        s += d2.toordinal() - EPOCH_ORDINAL
    return s


def main():
    run_id = env("PROFILE_RUN_ID", "local")
    ts = env("PROFILE_RUN_TS", "")
    warmup = int(env("PROFILE_WARMUP", "3"))
    trials = int(env("PROFILE_TRIALS", "10"))
    out = []

    s, ok = bench(warmup, trials, instant_arith, lambda r: r == INSTANT_REF)
    out.append(emit(run_id, ts, "time-instant-arith", N_INSTANT, warmup, trials, stats(s), ok))

    s, ok = bench(warmup, trials, localdate_arith, lambda r: r == DATE_REF)
    out.append(emit(run_id, ts, "time-localdate-arith", N_DATE, warmup, trials, stats(s), ok))

    sys.stdout.write("\n".join(out) + "\n")


if __name__ == "__main__":
    main()
