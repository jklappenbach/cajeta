#!/usr/bin/env python3
"""Time a command the way dev.cajeta.bench's ExternalBenchmark does, and print
one competitor row for a toolchain/ benchmark (bench/toolchain/*.md).

    cmdbench.py --bench <slug> --lang <l> --lang-version <v> --library <lib>
                [--library-version <v>] [--flags <f>] [--variant <v>]
                [--warmup N] [--trials N] [--input-size N] [--notes <s>]
                [--cwd DIR] [--env K=V ...]
                [--before SHELL]          untimed, before every execution
                [--expect-stdout TEXT]    the last trial's stdout must contain it
                [--verify SHELL --verify-expect TEXT]
                                          untimed, after the trials: its stdout
                                          must contain TEXT (e.g. run the binary
                                          a compile produced)
                [--size-of PATH]          binary-size rows: peak_rss_kb is the
                                          file's size on disk in KiB (rounded
                                          up), the exact bytes go in notes
                [--skip REASON]           print a skipped row and exit 0
                -- argv...

Each trial is one fork/exec/wait of argv, timed with perf_counter_ns around
Popen and wait4, exactly the region ExternalBenchmark times. peak_rss_kb is
taken the way ExternalBenchmark takes it: one extra untimed execution under
`/usr/bin/time -f %M` (the child's peak resident set from wait4 rusage). Not
from this process's own wait4: a child forked from Python starts with
Python's resident pages counted in its ru_maxrss until it execs, so every
small program would read about 13 MB. -1 without /usr/bin/time. --size-of
replaces it.
alloc_bytes and working_set_kb are -1 (they describe this process, which
only waits) and notes carry `external`, as on the Cajeta rows.

The row is schema v1 (32 columns); competitors/lib/rows.sh's rows_v2 maps it
to v2 (machine, os, platform). Commas in free text become semicolons.

PROFILE_WARMUP / PROFILE_TRIALS, when set, override --warmup / --trials (the
smokes set 1 and 3). PROFILE_RUN_ID and PROFILE_RUN_TS fill run_id and
timestamp_unix.
"""
import argparse
import math
import os
import statistics
import subprocess
import sys
import tempfile
import time


def clean(s):
    return (s or "").replace(",", ";").replace("\n", " ").replace("\r", " ").replace('"', "'").strip()


def row(a, warm, trials, samples, rss, status, ok, notes):
    rid = os.environ.get("PROFILE_RUN_ID", "local")
    ts = os.environ.get("PROFILE_RUN_TS", "") or str(int(time.time()))
    if samples:
        s = sorted(samples)
        mn, med, mean = s[0], int(statistics.median(s)), int(statistics.fmean(s))
        p95 = s[min(len(s) - 1, int(math.ceil(0.95 * len(s))) - 1)]
    else:
        mn = med = mean = p95 = -1
    thr = ""
    if a.input_size >= 0 and med > 0:
        thr = "%.3f" % (a.input_size * 1e9 / med)
    cols = ["1", rid, ts, a.bench, "toolchain", clean(a.dataset), "", "", str(a.input_size),
            a.lang, clean(a.lang_version), clean(a.library), clean(a.library_version), clean(a.flags),
            str(warm), str(trials), str(mn), str(med), str(mean), str(p95), thr, clean(a.unit),
            str(rss), "-1", "-1", "-1", "-1", status, ok, clean(notes), clean(a.variant), "cpu"]
    print(",".join(cols), flush=True)


def execute(argv, cwd, env, out, err):
    out.seek(0); out.truncate()
    err.seek(0); err.truncate()
    t0 = time.perf_counter_ns()
    p = subprocess.Popen(argv, cwd=cwd, env=env, stdout=out, stderr=err, stdin=subprocess.DEVNULL)
    _, status, ru = os.wait4(p.pid, 0)
    t1 = time.perf_counter_ns()
    p.returncode = os.waitstatus_to_exitcode(status)
    return t1 - t0, p.returncode, ru.ru_maxrss


def text(f):
    f.flush()
    f.seek(0)
    return f.read().decode("utf-8", "replace")


def shell(cmd, cwd, env):
    r = subprocess.run(["bash", "-c", cmd], cwd=cwd, env=env, capture_output=True)
    return r.returncode, r.stdout.decode("utf-8", "replace"), r.stderr.decode("utf-8", "replace")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bench", required=True)
    ap.add_argument("--lang", required=True)
    ap.add_argument("--lang-version", default="")
    ap.add_argument("--library", default="")
    ap.add_argument("--library-version", default="")
    ap.add_argument("--flags", default="")
    ap.add_argument("--variant", default="")
    ap.add_argument("--dataset", default="")
    ap.add_argument("--unit", default="")
    ap.add_argument("--warmup", type=int, default=3)
    ap.add_argument("--trials", type=int, default=20)
    ap.add_argument("--input-size", type=int, default=-1)
    ap.add_argument("--notes", default="")
    ap.add_argument("--cwd", default=None)
    ap.add_argument("--env", action="append", default=[])
    ap.add_argument("--before", default=None)
    ap.add_argument("--expect-stdout", default=None)
    ap.add_argument("--verify", default=None)
    ap.add_argument("--verify-expect", default=None)
    ap.add_argument("--size-of", default=None)
    ap.add_argument("--skip", default=None)
    ap.add_argument("argv", nargs=argparse.REMAINDER)
    a = ap.parse_args()
    argv = a.argv[1:] if a.argv and a.argv[0] == "--" else a.argv

    warm = int(os.environ.get("PROFILE_WARMUP") or a.warmup)
    trials = max(1, int(os.environ.get("PROFILE_TRIALS") or a.trials))
    notes = "external" + ("; " + a.notes if a.notes else "")

    if a.skip:
        row(a, 0, 0, [], -1, "skipped", "", notes + "; " + a.skip)
        return
    if not argv:
        sys.exit("cmdbench.py: no command")

    env = dict(os.environ)
    for kv in a.env:
        k, _, v = kv.partition("=")
        env[k] = v

    samples, rss = [], -1
    with tempfile.TemporaryFile() as out, tempfile.TemporaryFile() as err:
        for i in range(warm + trials):
            if a.before:
                rc, _, e = shell(a.before, a.cwd, env)
                if rc != 0:
                    row(a, warm, trials, [], -1, "failed", "false",
                        notes + "; setup step exited %d: %s" % (rc, (e.strip().splitlines() or [""])[0]))
                    return
            try:
                ns, rc, mr = execute(argv, a.cwd, env, out, err)
            except OSError as e:
                row(a, warm, trials, [], -1, "failed", "false", notes + "; could not start %s: %s" % (argv[0], e))
                return
            if rc != 0:
                first = (text(err).strip().splitlines() or [""])[0]
                row(a, warm, trials, [], -1, "failed", "false",
                    notes + "; %s exited %d: %s" % (os.path.basename(argv[0]), rc, first[:160]))
                return
            if i >= warm:
                samples.append(ns)
        stdout = text(out)
        if os.path.exists("/usr/bin/time") and not a.size_of:
            if a.before:
                shell(a.before, a.cwd, env)
            try:
                r = subprocess.run(["/usr/bin/time", "-f", "CAJETA_BENCH_MAXRSS=%M"] + argv, cwd=a.cwd,
                                   env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                                   stdin=subprocess.DEVNULL)
                for ln in r.stderr.decode("utf-8", "replace").splitlines():
                    if ln.startswith("CAJETA_BENCH_MAXRSS="):
                        rss = int(ln.split("=", 1)[1])
            except (OSError, ValueError):
                rss = -1

    ok = True
    why = ""
    if a.expect_stdout is not None and a.expect_stdout not in stdout:
        ok, why = False, "stdout lacks %r" % a.expect_stdout
    if ok and a.verify:
        rc, vout, verr = shell(a.verify, a.cwd, env)
        if rc != 0 or (a.verify_expect is not None and a.verify_expect not in vout):
            ok, why = False, "check exited %d: %s" % (rc, (vout + verr).strip()[:160])
    if a.size_of:
        try:
            size = os.path.getsize(a.size_of if os.path.isabs(a.size_of) or not a.cwd
                                   else os.path.join(a.cwd, a.size_of))
            rss = (size + 1023) // 1024
            notes += "; peak_rss_kb holds the size on disk in KiB (%d bytes); time is one launch" % size
        except OSError as e:
            ok, why = False, "cannot stat %s: %s" % (a.size_of, e)
    if not ok:
        notes += "; " + why
    row(a, warm, trials, samples, rss, "ok" if ok else "invalid", "true" if ok else "false", notes)


if __name__ == "__main__":
    main()
