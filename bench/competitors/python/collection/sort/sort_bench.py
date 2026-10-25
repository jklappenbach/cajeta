#!/usr/bin/env python3
# Python collection/ sort competitors: sort-int64 (random, ascending,
# descending, dups), sort-f64 and sort-stable-int64 at the page shape (1e6;
# n/100 under CAJETA_BENCH_SMOKE=1), same inputs and checks as the Cajeta
# benchmarks (CollGen): random = key(i), f64 = draw / 2^31, stable = (value,
# index) pairs packed value << 20 | index with value = draw % 1e5, sorted on
# the value. CPython has one sort, Timsort (list.sort / sorted). The copy of
# the input is outside the time (the Cajeta side subtracts its copy).
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "common"))
from collgen import Gen, key, params, row, selected, size_note, sized  # noqa: E402

W, T = params()
FULL = 1_000_000
N = sized(FULL)


def is_sorted(a):
    return all(a[i - 1] <= a[i] for i in range(1, len(a)))


def run(bench_id, variant, src, keyf=None, notes=""):
    if not selected(bench_id):
        return
    for _ in range(W):
        w = list(src)
        w.sort(key=keyf)
    samples = []
    w = None
    for _ in range(T):
        w = list(src)
        t = time.perf_counter_ns()
        w.sort(key=keyf)
        samples.append(time.perf_counter_ns() - t)
    print(row(bench_id, "list.sort (Timsort)", "cpython", N, W, T, samples, len(w) == N and is_sorted(w),
              size_note(FULL, notes), variant), flush=True)


for v in ("random", "ascending", "descending", "dups"):
    if v == "random":
        src = [key(i) for i in range(N)]
    elif v == "ascending":
        src = list(range(N))
    elif v == "descending":
        src = list(range(N - 1, -1, -1))
    else:
        src = [i % 16 for i in range(N)]
    run("sort-int64", v, src)
g = Gen()
run("sort-f64", "", [g.next() / 2147483648.0 for _ in range(N)])
g = Gen()
run("sort-stable-int64", "", [((g.next() % 100000) << 20) | i for i in range(N)], keyf=lambda x: x >> 20,
    notes="(value index) pairs packed in an int; key on the value")
