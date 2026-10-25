"""Row helpers for the codec competitor programs (v1 31-column rows; see competitors/lib/rows.sh)."""
import os
import resource
import sys
import time

FLAGS = "CPython as shipped"


def env(k, d):
    v = os.environ.get(k)
    return v if v else d


RUN_ID = env("PROFILE_RUN_ID", "local")
TS = env("PROFILE_RUN_TS", "")
PYVER = env("PROFILE_LANG_VERSION", "")


def peak_rss_kb():
    return resource.getrusage(resource.RUSAGE_SELF).ru_maxrss


def clean(s):
    return str(s).replace(",", ";").replace("\n", " ")


def measure(fn, warmup, trials):
    """Run fn warmup+trials times; returns (stats, last_result)."""
    res = None
    for _ in range(warmup):
        res = fn()
    samples = []
    for _ in range(trials):
        t0 = time.perf_counter_ns()
        res = fn()
        samples.append(time.perf_counter_ns() - t0)
    s = sorted(samples)
    n = len(s)
    st = (s[0], s[n // 2], sum(s) // n, s[min(n - 1, n * 95 // 100)])
    return st, res


def row(slug, dataset, dbytes, isize, lib, ver, warmup, trials, st, ok, notes, variant, mbps=True):
    mn, med, mean, p95 = st
    thr, unit = "", ""
    if mbps:
        thr = f"{(isize / med * 1e9 / 1048576.0) if med > 0 else 0.0:.1f}"
        unit = "MB/s"
    status = "ok" if ok else "invalid"
    return (f"1,{RUN_ID},{TS},{slug},codec,{dataset},{dbytes},,{isize},python,{PYVER},"
            f"{lib},{ver},{FLAGS},{warmup},{trials},{mn},{med},{mean},{p95},{thr},{unit},"
            f"{peak_rss_kb()},-1,-1,-1,-1,{status},{str(bool(ok)).lower()},{clean(notes)},{variant}")


def skip(slug, dataset, dbytes, isize, lib, ver, reason, variant):
    return (f"1,{RUN_ID},{TS},{slug},codec,{dataset},{dbytes},,{isize},python,{PYVER},"
            f"{lib},{ver},{FLAGS},0,0,-1,-1,-1,-1,,,-1,-1,-1,-1,-1,skipped,,{clean(reason)},{variant}")


def out(line):
    sys.stdout.write(line + "\n")
    sys.stdout.flush()
