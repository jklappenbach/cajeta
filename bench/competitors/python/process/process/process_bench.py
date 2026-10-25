"""process/ competitor in Python (bench/process/Command.md, Process.md): subprocess.

command-spawn-wait   subprocess.run(["/bin/true"]) R = 1e3 times (smoke 1e2); check: every returncode 0
process-pipe-100mb   100 MiB (smoke 10 MiB) of data[i] = (i*31+7) & 0xFF through `cat` with
                     subprocess.run(input=data, stdout=PIPE) (communicate() writes and reads concurrently);
                     check: length (every trial) and byte sum (last trial, untimed) equal the input's.
Rows are v1 (31 columns); competitors/lib/rows.sh lifts them to v2.
"""
import os
import platform
import resource
import subprocess
import sys
import time


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


def row(slug, isize, w, t, st, ok, notes, mbps):
    thr, unit = "", ""
    if mbps and st[1] > 0:
        thr, unit = f"{isize / st[1] * 1e9 / 1048576.0:.1f}", "MB/s"
    notes = str(notes).replace(",", ";")
    print(f"1,{env('PROFILE_RUN_ID', 'local')},{env('PROFILE_RUN_TS', '')},{slug},process,,0,,{isize},python,"
          f"{env('PROFILE_LANG_VERSION', 'Python ' + platform.python_version())},subprocess,{platform.python_version()},"
          f"CPython as shipped,{w},{t},{st[0]},{st[1]},{st[2]},{st[3]},{thr},{unit},"
          f"{resource.getrusage(resource.RUSAGE_SELF).ru_maxrss},-1,-1,-1,-1,{'ok' if ok else 'invalid'},"
          f"{'true' if ok else 'false'},{notes},", flush=True)


def main():
    warmup = int(env("PROFILE_WARMUP", "1"))
    trials = max(1, int(env("PROFILE_TRIALS", "5")))
    sm = env("CAJETA_BENCH_SMOKE", "") in ("1", "true", "yes")

    r = 100 if sm else 1000
    res = {}

    def spawn():
        z = 0
        for _ in range(r):
            if subprocess.run(["/bin/true"]).returncode == 0:
                z += 1
        res["zero"] = z

    st = measure(spawn, warmup, trials)
    n1 = f"subprocess.run([/bin/true]) x{r} per trial; check: zero exits == {r}"
    if sm:
        n1 += "; smoke size 1e2 spawns in place of 1e3"
    row("command-spawn-wait", r, warmup, trials, st, res["zero"] == r, n1, False)

    n = (10 << 20) if sm else (100 << 20)
    pattern = bytes(((i * 31 + 7) & 0xFF) for i in range(256))
    data = (pattern * (n // 256 + 1))[:n]
    want = sum(pattern) * (n // 256) + sum(data[(n // 256) * 256:])

    def pipe():
        p = subprocess.run(["cat"], input=data, stdout=subprocess.PIPE)
        res["out"] = p.stdout if p.returncode == 0 else b""
        if len(res["out"]) != n:
            res["badlen"] = res.get("badlen", 0) + 1

    sp = measure(pipe, warmup, trials)
    res["sum"] = sum(res["out"])
    n2 = (f"subprocess.run([cat], input=data, stdout=PIPE) (communicate); the byte sum of the last trial's output "
          f"is taken after timing (a CPython byte loop would dominate the row); "
          f"check: length {n} and byte sum {want}")
    if sm:
        n2 += "; smoke size 10 MiB in place of 100 MiB"
    row("process-pipe-100mb", n, warmup, trials, sp, res.get("badlen", 0) == 0 and len(res["out"]) == n and res["sum"] == want, n2, True)


if __name__ == "__main__":
    sys.exit(main())
