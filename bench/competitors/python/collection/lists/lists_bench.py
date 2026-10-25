#!/usr/bin/env python3
# Python collection/ competitors, sequence half: the ArrayList, LinkedList,
# Heap, ImmutableList, Collectors and binary-search ids, same sizes, inputs and
# checks as the Cajeta benchmarks (bench/src/main/cajeta/bench/collection/).
# Libraries: list, collections.deque, heapq, tuple, list comprehension,
# collections.defaultdict, bisect. One v1 row per (id, library).
import bisect
import heapq
import os
import sys
from collections import defaultdict, deque

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "common"))
from collgen import Gen, bench, params, row, selected  # noqa: E402

W, T = params()
out = []


def run(bench_id, lib, libver, size, f, check, notes=""):
    if not selected(bench_id):
        return
    s, ok = bench(W, T, f, check)
    out.append(row(bench_id, lib, libver, size, W, T, s, ok, notes))
    print(out[-1], flush=True)


N1M = 1_000_000
lst1m = list(range(N1M))
idx1m = Gen().indices(N1M, N1M)
exp_get = sum(idx1m) * 100


def get_loop(seq):
    def f():
        s = 0
        for _ in range(100):
            for j in idx1m:
                s += seq[j]
        return s
    return f


# arraylist-get: 1e8 random gets (a 1e6 index table walked 100 times) on a 1e6 list.
run("arraylist-get", "list", "cpython", 100_000_000, get_loop(lst1m), lambda r: r == exp_get)

# arraylist-iterate: foreach over 1e7 elements, 10 times.
lst1e7 = list(range(10_000_000))


def iterate():
    s = 0
    for _ in range(10):
        for v in lst1e7:
            s += v
    return s


run("arraylist-iterate", "list", "cpython", 100_000_000, iterate,
    lambda r: r == 10 * (10_000_000 * 9_999_999 // 2), "for v in list")

# arraylist-sort: copy 1e6 draws into a fresh list and sort() it (Timsort).
g = Gen()
src_sort = [g.next() for _ in range(N1M)]


def sort_once():
    a = list(src_sort)
    a.sort()
    return a


run("arraylist-sort", "list.sort", "cpython", N1M, sort_once,
    lambda a: len(a) == N1M and all(a[i - 1] <= a[i] for i in range(1, len(a))), "includes the copy")

# linkedlist-insert-traverse (the Cajeta shape): append 40000, count, contains(n-1).
N_LL = 40000


def ll():
    d = deque()
    for i in range(N_LL):
        d.append(i)
    return len(d), (N_LL - 1) in d


run("linkedlist-insert-traverse", "collections.deque", "cpython", N_LL, ll, lambda r: r == (N_LL, True))

# heap-sort (the Cajeta shape): push 50000 LCG values, pop all, monotonic.
N_HS = 50000


def heap_sort():
    h = []
    x = 12345
    for _ in range(N_HS):
        x = (x * 1103515245 + 12345) & 0x7FFFFFFF
        heapq.heappush(h, x)
    out_ = [heapq.heappop(h) for _ in range(N_HS)]
    return out_


run("heap-sort", "heapq", "cpython", N_HS, heap_sort,
    lambda a: all(a[i - 1] <= a[i] for i in range(1, len(a))))

# heap-push-pop: min-heap of 1e3 draws, then 1e7 push + pop, summing pops.
g = Gen()
hp_vals = [g.next() for _ in range(10_001_000)]


def push_pop():
    h = hp_vals[:1000]
    heapq.heapify(h)
    c = 0
    push, pop = heapq.heappush, heapq.heappop
    for v in hp_vals[1000:]:
        push(h, v)
        c += pop(h)
    return c


run("heap-push-pop", "heapq", "cpython", 10_000_000, push_pop, lambda c: c == 10738394301026345,
    "heappush then heappop")

# immutablelist-build: tuple(list) of 1e6, 100 times.


def il_build():
    t = 0
    last = None
    for _ in range(100):
        last = tuple(lst1m)
        t += len(last)
    return t, last[999_999]


run("immutablelist-build", "tuple", "cpython", 100_000_000, il_build, lambda r: r == (100_000_000, 999_999))

# immutablelist-get: the arraylist-get walk over a tuple.
tup1m = tuple(lst1m)
run("immutablelist-get", "tuple", "cpython", 100_000_000, get_loop(tup1m), lambda r: r == exp_get)

# collectors-to-list: list comprehension over 1e7.


def to_list():
    o = [x for x in lst1e7]
    return len(o), o[-1]


run("collectors-to-list", "list comprehension", "cpython", 10_000_000, to_list, lambda r: r == (10_000_000, 9_999_999))

# collectors-group-by: defaultdict(list) keyed by x % 1000.


def group_by():
    d = defaultdict(list)
    for x in lst1e7:
        d[x % 1000].append(x)
    return len(d), len(d[0])


run("collectors-group-by", "collections.defaultdict", "cpython", 10_000_000, group_by, lambda r: r == (1000, 10000))

# binary-search (the Cajeta shape): a[i] = 2i over 50000, 50000 bisect lookups.
N_BS = 50000


def bsearch():
    a = [i * 2 for i in range(N_BS)]
    hits = 0
    x = 12345
    for _ in range(N_BS):
        x = (x * 1103515245 + 12345) & 0x7FFFFFFF
        k = (x % N_BS) * 2
        i = bisect.bisect_left(a, k)
        if i < N_BS and a[i] == k:
            hits += 1
    return hits


run("binary-search", "bisect", "cpython", N_BS, bsearch, lambda h: h == N_BS)
