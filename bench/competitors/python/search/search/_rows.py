"""Row helpers for the search/ competitor program (v1 31-column rows; see competitors/lib/rows.sh)."""
import os
import resource
import sys
import time

FLAGS = "CPython as shipped"
AREA = "search"


def env(k, d):
    v = os.environ.get(k)
    return v if v else d


RUN_ID = env("PROFILE_RUN_ID", "local")
TS = env("PROFILE_RUN_TS", "")
PYVER = env("PROFILE_LANG_VERSION", "")


def smoke():
    return env("CAJETA_BENCH_SMOKE", "") in ("1", "true", "yes")


def peak_rss_kb():
    return resource.getrusage(resource.RUSAGE_SELF).ru_maxrss


def clean(s):
    return str(s).replace(",", ";").replace("\n", " ")


def measure(fn, warmup, trials):
    res = None
    for _ in range(warmup):
        res = fn()
    samples = []
    for _ in range(max(1, trials)):
        t0 = time.perf_counter_ns()
        res = fn()
        samples.append(time.perf_counter_ns() - t0)
    s = sorted(samples)
    n = len(s)
    return (s[0], s[n // 2], sum(s) // n, s[min(n - 1, n * 95 // 100)]), res


def row(slug, isize, lib, ver, warmup, trials, st, ok, notes, unit="items/s", variant=""):
    mn, med, mean, p95 = st
    thr = f"{isize / med * 1e9:.1f}" if med > 0 else ""
    status = "ok" if ok else "invalid"
    out(f"1,{RUN_ID},{TS},{slug},{AREA},,0,,{isize},python,{clean(PYVER)},"
        f"{lib},{ver},{FLAGS},{warmup},{trials},{mn},{med},{mean},{p95},{thr},{unit},"
        f"{peak_rss_kb()},-1,-1,-1,-1,{status},{str(bool(ok)).lower()},{clean(notes)},{variant}")


def out(line):
    sys.stdout.write(line + "\n")
    sys.stdout.flush()
