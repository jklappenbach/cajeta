# The collection/ competitors' shared inputs and row format (Python side).
# Mirrors bench/src/main/cajeta/bench/collection/CollGen.cajeta exactly: the
# same draws in the same order give the same keys, queries and checks in all
# six languages.
#   next(): s ^= s << 13; s ^= s >> 7; s ^= s << 17 (mod 2^64); return s >> 33
#           (xorshift64, seed 88172645463325252; a 31-bit draw)
#   key(i): three xorshift64 rounds of i ^ 0x5851F42D4C957F2D, read as signed int64
import os
import resource
import time

M64 = (1 << 64) - 1
SEED = 88172645463325252


def xs(x):
    x ^= (x << 13) & M64
    x ^= x >> 7
    x ^= (x << 17) & M64
    return x


def key(i):
    x = (i ^ 0x5851F42D4C957F2D) & M64
    x = xs(xs(xs(x)))
    return x - (1 << 64) if x >= (1 << 63) else x


class Gen:
    def __init__(self):
        self.s = SEED

    def next(self):
        self.s = xs(self.s)
        return self.s >> 33

    def indices(self, count, rng):
        out = [0] * count
        s = self.s
        for i in range(count):
            s ^= (s << 13) & M64
            s ^= s >> 7
            s ^= (s << 17) & M64
            out[i] = (s >> 33) % rng
        self.s = s
        return out


def env(k, d):
    v = os.environ.get(k)
    return v if v else d


def peak_rss_kb():
    return resource.getrusage(resource.RUSAGE_SELF).ru_maxrss


def bench(warmup, trials, f, check):
    """Warm up, then time `trials` calls of f; check the last result (untimed)."""
    for _ in range(warmup):
        f()
    samples = []
    r = None
    for _ in range(trials):
        t = time.perf_counter_ns()
        r = f()
        samples.append(time.perf_counter_ns() - t)
    return samples, bool(check(r))


def row(bench_id, lib, libver, input_size, warmup, trials, samples, ok, notes="", variant=""):
    s = sorted(samples)
    n = len(s)
    mn, med, mean, p95 = s[0], s[n // 2], sum(s) // n, s[min(n - 1, n * 95 // 100)]
    mops = (input_size / med * 1e3) if med > 0 else 0.0
    status = "ok" if ok else "invalid"
    note = "alloc not measured (tracemalloc would multiply the run time)" + (f"; {notes}" if notes else "")
    return (f"1,{env('PROFILE_RUN_ID', 'local')},{env('PROFILE_RUN_TS', '')},{bench_id},collection,,{input_size},,"
            f"{input_size},python,{env('PROFILE_LANG_VERSION', '')},{lib},{libver},-OO,"
            f"{warmup},{trials},{mn},{med},{mean},{p95},{mops:.2f},Mop/s,{peak_rss_kb()},"
            f"-1,-1,-1,-1,{status},{str(ok).lower()},{note.replace(',', ' ')},{variant}")


def skip(bench_id, lib, input_size, reason):
    return (f"1,{env('PROFILE_RUN_ID', 'local')},{env('PROFILE_RUN_TS', '')},{bench_id},collection,,{input_size},,"
            f"{input_size},python,{env('PROFILE_LANG_VERSION', '')},{lib},,,0,0,-1,-1,-1,-1,,,-1,-1,-1,-1,-1,"
            f"skipped,,{reason.replace(',', ' ')},")


def params():
    return int(env("PROFILE_WARMUP", "3")), int(env("PROFILE_TRIALS", "10"))


def selected(bench_id):
    """PROFILE_BENCH (space list) narrows a program to some of its ids."""
    sel = os.environ.get("PROFILE_BENCH", "").split()
    return not sel or bench_id in sel


def _flag(k):
    return os.environ.get(k, "") in ("1", "true", "yes")


def small_smoke():
    """CAJETA_BENCH_SMOKE=1 runs the first-suite rows at n/100 (CollGen.sized on the
    Cajeta side); CAJETA_BENCH_FULL_SIZE=1 keeps the page shape."""
    return _flag("CAJETA_BENCH_SMOKE") and not _flag("CAJETA_BENCH_FULL_SIZE")


def sized(full):
    return full // 100 if small_smoke() else full


def size_note(full, extra=""):
    parts = [extra] if extra else []
    if small_smoke():
        parts.append(f"smoke size n/100 (page shape {full})")
    return "; ".join(parts)


def hex16(v):
    return format(v & M64, "016x")
