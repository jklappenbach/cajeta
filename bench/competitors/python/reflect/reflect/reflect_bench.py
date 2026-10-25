"""reflect/ competitor in Python (bench/reflect/Class.md).

class-lookup     getattr(module, name) 1e7 times (smoke 1e6), cycling over 4 class names: lookup by name in
                 the module namespace, then isinstance(c, type); the page's "Python type" read as a by-name
                 class lookup (type(obj) is not a lookup by name). Check: found == n.
field-enumerate  vars(obj) (the instance __dict__) of a 20-attribute object (a0..a19: 10 int, 5 int, 5 float;
                 Python ints are unbounded, so the int64/int32 split is only nominal) 1e6 times (smoke 1e5),
                 counting its keys and their name lengths; check: fields == 20n and name bytes == 50n.
Rows are v1 (31 columns); competitors/lib/rows.sh lifts them to v2.
"""
import os
import platform
import resource
import sys
import time


class RT0:
    pass


class RT1:
    pass


class RT2:
    pass


class RT3:
    pass


class Target20:
    def __init__(self):
        for i in range(20):
            setattr(self, f"a{i}", 0 if i < 15 else 0.0)


def env(k, d):
    v = os.environ.get(k)
    return v if v else d


def measure(fn, warmup, trials):
    for _ in range(warmup):
        fn()
    s = []
    for _ in range(trials):
        t0 = time.perf_counter_ns()
        fn()
        s.append(time.perf_counter_ns() - t0)
    s.sort()
    n = len(s)
    return (s[0], s[n // 2], sum(s) // n, s[min(n - 1, n * 95 // 100)])


def row(slug, isize, w, t, st, ok, notes):
    notes = str(notes).replace(",", ";")
    print(f"1,{env('PROFILE_RUN_ID', 'local')},{env('PROFILE_RUN_TS', '')},{slug},reflect,,0,,{isize},python,"
          f"{env('PROFILE_LANG_VERSION', 'Python ' + platform.python_version())},builtins,{platform.python_version()},"
          f"CPython as shipped,{w},{t},{st[0]},{st[1]},{st[2]},{st[3]},,,"
          f"{resource.getrusage(resource.RUSAGE_SELF).ru_maxrss},-1,-1,-1,-1,{'ok' if ok else 'invalid'},"
          f"{'true' if ok else 'false'},{notes},", flush=True)


def main():
    warmup = int(env("PROFILE_WARMUP", "1"))
    trials = max(1, int(env("PROFILE_TRIALS", "5")))
    sm = env("CAJETA_BENCH_SMOKE", "") in ("1", "true", "yes")
    mod = sys.modules[__name__]
    res = {}

    n = 1000000 if sm else 10000000
    names = ("RT0", "RT1", "RT2", "RT3")

    def lookup():
        f = 0
        for i in range(n):
            if isinstance(getattr(mod, names[i & 3]), type):
                f += 1
        res["found"] = f

    st = measure(lookup, warmup, trials)
    n1 = f"getattr(module; name) + isinstance(c; type) x{n} over 4 names; check: found == {n}"
    if sm:
        n1 += "; smoke size 1e6 in place of 1e7"
    row("class-lookup", n, warmup, trials, st, res["found"] == n, n1)

    m = 100000 if sm else 1000000
    obj = Target20()

    def enumerate_fields():
        fc = 0
        bc = 0
        for _ in range(m):
            for k in vars(obj):
                fc += 1
                bc += len(k)
        res["f"] = fc
        res["b"] = bc

    sf = measure(enumerate_fields, warmup, trials)
    n2 = f"vars(obj) keys (instance __dict__) of a 20-attribute object x{m}; check: fields == {20 * m} and name bytes == {50 * m}"
    if sm:
        n2 += "; smoke size 1e5 in place of 1e6"
    row("field-enumerate", m, warmup, trials, sf, res["f"] == 20 * m and res["b"] == 50 * m, n2)


if __name__ == "__main__":
    main()
