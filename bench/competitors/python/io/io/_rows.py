"""Row helpers for the Python io competitor (v1 31-column rows, area io; see
competitors/lib/rows.sh) and the shared sizes."""
import os
import resource
import sys
import time

FLAGS = "CPython as shipped"


def env(k, d):
    v = os.environ.get(k)
    return v if v else d


def smoke():
    return env("CAJETA_BENCH_SMOKE", "") in ("1", "true", "yes")


def pick(full, sm_):
    return sm_ if smoke() else full


def env_int(k, d):
    try:
        return int(env(k, str(d)))
    except ValueError:
        return d


def warm(d):
    return 1 if smoke() else env_int("PROFILE_WARMUP", d)


def trials(d):
    return env_int("PROFILE_TRIALS", 3) if smoke() else env_int("PROFILE_TRIALS", d)


def sm(what):
    return f" (smoke; the page's {what} is the full run)" if smoke() else ""


RUN_ID = env("PROFILE_RUN_ID", "local")
TS = env("PROFILE_RUN_TS", "")
PYVER = env("PROFILE_LANG_VERSION", "")


def peak_rss_kb():
    return resource.getrusage(resource.RUSAGE_SELF).ru_maxrss


def rss_kb():
    with open("/proc/self/status") as f:
        for line in f:
            if line.startswith("VmRSS:"):
                return int(line.split()[1])
    return -1


def cpu_ms():
    """utime + stime in milliseconds (USER_HZ = 100)."""
    with open("/proc/self/stat") as f:
        s = f.read()
    fl = s[s.rindex(")") + 2:].split()
    return (int(fl[11]) + int(fl[12])) * 10


def clean(s):
    return str(s).replace(",", ";").replace("\n", " ")


def measure(fn, warmup, trials_):
    for _ in range(warmup):
        fn()
    samples = []
    for _ in range(max(1, trials_)):
        t0 = time.perf_counter_ns()
        fn()
        samples.append(time.perf_counter_ns() - t0)
    s = sorted(samples)
    n = len(s)
    return (s[0], s[n // 2], sum(s) // n, s[min(n - 1, n * 95 // 100)])


def row(slug, isize, lib, ver, warmup, trials_, st, ok, notes, unit):
    mn, med, mean, p95 = st
    thr, u = "", ""
    if unit and med > 0 and isize >= 0:
        thr = f"{isize * 1e9 / med:.3f}"
        u = unit
    status = "ok" if ok else "invalid"
    nt = clean(notes) + ("; smoke" if smoke() else "")
    return (f"1,{RUN_ID},{TS},{slug},io,,-1,,{isize},python,{PYVER},"
            f"{lib},{ver},{FLAGS},{warmup},{trials_},{mn},{med},{mean},{p95},{thr},{u},"
            f"{peak_rss_kb()},-1,-1,-1,-1,{status},{str(bool(ok)).lower()},{nt},")


def err(slug, lib, e):
    return (f"1,{RUN_ID},{TS},{slug},io,,-1,,-1,python,{PYVER},{lib},,{FLAGS},0,0,-1,-1,-1,-1,,,"
            f"-1,-1,-1,-1,-1,invalid,false,{clean('error: ' + repr(e))},")


def out(line):
    sys.stdout.write(line + "\n")
    sys.stdout.flush()
