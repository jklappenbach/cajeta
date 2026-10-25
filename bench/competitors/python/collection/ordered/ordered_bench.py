#!/usr/bin/env python3
# Python collection/ competitors, ordered half: redblacktree-insert-lookup (the
# Cajeta shape), redblacktree-range, bplustree-insert, bplustree-point-lookup,
# bplustree-range, same sizes, inputs and checks as the Cajeta benchmarks.
# Library: sortedcontainers.SortedDict (pinned in competitors/python/
# requirements.txt). SortedDict answers a point lookup from its backing dict,
# so its point-lookup rows are hash lookups; its ranges walk the sorted key list.
import bisect
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "common"))
from collgen import Gen, bench, key, params, row, selected, size_note, sized, skip  # noqa: E402

try:
    import sortedcontainers
    from sortedcontainers import SortedDict
except ImportError:
    for b, n in (("redblacktree-insert-lookup", 1_000_000), ("redblacktree-range", 10_000_000),
                 ("bplustree-insert", 1_000_000), ("bplustree-point-lookup", 10_000_000),
                 ("bplustree-range", 10_000_000)):
        print(skip(b, "sortedcontainers", n, "sortedcontainers not installed (run competitors/setup.sh)"))
    sys.exit(0)

VER = sortedcontainers.__version__
W, T = params()
N = 1_000_000


def run(bench_id, size, f, check, notes=""):
    if not selected(bench_id):
        return
    s, ok = bench(W, T, f, check)
    print(row(bench_id, "sortedcontainers.SortedDict", VER, size, W, T, s, ok, notes), flush=True)


# redblacktree-insert-lookup: insert 1e6 random int keys key(i) -> i, look each up.
N_RB = sized(N)
rb_keys = [key(i) for i in range(N_RB)]


def rb():
    t = SortedDict()
    for i, k in enumerate(rb_keys):
        t[k] = i
    s = 0
    for k in rb_keys:
        s += t[k]
    return s


run("redblacktree-insert-lookup", N_RB, rb, lambda s: s == N_RB * (N_RB - 1) // 2, size_note(N))
del rb_keys

keys = [key(i) for i in range(N)]


def insert():
    t = SortedDict()
    for i, k in enumerate(keys):
        t[k] = i
    return len(t), t[keys[7]]


run("bplustree-insert", N, insert, lambda r: r == (N, 7))

tree = SortedDict(zip(keys, range(N)))
idx = Gen().indices(10_000_000, N)
q = [keys[i] for i in idx]
exp = sum(idx)
del idx


def point():
    s = 0
    for k in q:
        s += tree[k]
    return s


run("bplustree-point-lookup", 10_000_000, point, lambda s: s == exp, "SortedDict lookups are dict lookups")

# Ranges: 1e4 scans of 1e3 entries from the first key >= key(draw % 1e6).
starts = [key(i) for i in Gen().indices(10_000, N)]
sk = sorted(keys)
sv = [tree[k] for k in sk]
range_exp = 0
for st_ in starts:
    p = bisect.bisect_left(sk, st_)
    range_exp += sum(sv[p:p + 1000])
del sk, sv


def scans():
    s = 0
    for st_ in starts:
        c = 0
        for k in tree.irange(minimum=st_):
            s += tree[k]
            c += 1
            if c == 1000:
                break
    return s


for b in ("redblacktree-range", "bplustree-range"):
    run(b, 10_000_000, scans, lambda s: s == range_exp, "irange from the start key then a value read per key")
