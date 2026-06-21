#!/usr/bin/env python3
# Python codec competitors — parse the nativejson corpus to a Python object DOM
# and emit one schema-conformant CSV row per (file, library), matching
# competitors/columns.txt. Libraries: json (stdlib), orjson, msgspec, ujson.
# A library that isn't importable is recorded as a skip row (never a crash).
# Cross-check: a known element count per file.
import importlib
import os
import resource
import sys
import time
import tracemalloc

FILES = [
    ("twitter", "twitter.json", 100),
    ("citm_catalog", "citm_catalog.json", 184),
    ("canada", "canada.json", 1),
]


def env(k, d):
    v = os.environ.get(k)
    return v if v else d


def peak_rss_kb():
    # ru_maxrss is KB on Linux.
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


def count(v, dataset):
    try:
        if dataset == "twitter":
            return len(v["statuses"])
        if dataset == "citm_catalog":
            return len(v["events"])
        if dataset == "canada":
            return len(v["features"])
    except (KeyError, TypeError):
        return 0
    return 0


def stats(samples):
    s = sorted(samples)
    n = len(s)
    idx = min(n - 1, n * 95 // 100)
    return s[0], s[n // 2], sum(s) // n, s[idx]


def row(run_id, ts, dataset, nbytes, lib, ver, warmup, trials, st, check_ok, status, ver_flags="-OO"):
    mn, med, mean, p95 = st
    mbps = (nbytes / med * 1e9 / 1048576.0) if med > 0 else 0.0
    pyver = env("PROFILE_LANG_VERSION", "")
    return (
        f"1,{run_id},{ts},json-dom,codec,{dataset},{nbytes},,{nbytes},python,{pyver},"
        f"{lib},{ver},{ver_flags},{warmup},{trials},{mn},{med},{mean},{p95},"
        f"{mbps:.1f},MB/s,{peak_rss_kb()},-1,{_ALLOC},-1,-1,{status},{str(check_ok).lower()},,"
    )


def skip_row(run_id, ts, dataset, nbytes, lib, reason):
    pyver = env("PROFILE_LANG_VERSION", "")
    return (
        f"1,{run_id},{ts},json-dom,codec,{dataset},{nbytes},,{nbytes},python,{pyver},"
        f"{lib},,,0,0,-1,-1,-1,-1,,,-1,-1,-1,-1,-1,skipped,,{reason},"
    )


def loader(lib):
    """Return (loads_callable, version) for a library, or None if unavailable."""
    try:
        if lib == "json":
            import json
            return (lambda b: json.loads(b), sys.version.split()[0])
        if lib == "orjson":
            orjson = importlib.import_module("orjson")
            return (orjson.loads, getattr(orjson, "__version__", ""))
        if lib == "msgspec":
            msgspec = importlib.import_module("msgspec")
            dec = msgspec.json.decode
            return (lambda b: dec(b), getattr(msgspec, "__version__", ""))
        if lib == "ujson":
            ujson = importlib.import_module("ujson")
            return (ujson.loads, getattr(ujson, "__version__", ""))
    except ImportError:
        return None
    return None


def main():
    global _ALLOC
    data_dir = os.environ.get("PROFILE_DATA_DIR")
    if not data_dir:
        sys.stderr.write("python/codec: PROFILE_DATA_DIR unset — skipping\n")
        return
    run_id = env("PROFILE_RUN_ID", "local")
    ts = env("PROFILE_RUN_TS", "")
    warmup = int(env("PROFILE_WARMUP", "3"))
    trials = int(env("PROFILE_TRIALS", "10"))

    libs = ["json", "orjson", "msgspec", "ujson"]
    out = []
    for dataset, fname, expect in FILES:
        path = os.path.join(data_dir, fname)
        try:
            with open(path, "rb") as f:
                raw = f.read()
        except OSError:
            continue  # dataset absent → skip
        n = len(raw)

        for lib in libs:
            ld = loader(lib)
            if ld is None:
                out.append(skip_row(run_id, ts, dataset, n, lib, f"{lib} not importable"))
                continue
            loads, ver = ld
            try:
                check = count(loads(raw), dataset) == expect
            except Exception:
                check = False
            _ALLOC = alloc_bytes(lambda: loads(raw))
            for _ in range(warmup):
                loads(raw)
            samples = []
            for _ in range(trials):
                t0 = time.perf_counter_ns()
                loads(raw)
                samples.append(time.perf_counter_ns() - t0)
            status = "ok" if check else "invalid"
            out.append(row(run_id, ts, dataset, n, lib, ver, warmup, trials,
                           stats(samples), check, status))

    sys.stdout.write("\n".join(out) + "\n")


if __name__ == "__main__":
    main()
