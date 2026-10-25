#!/usr/bin/env python3
# Python collection/ competitors, hash half: hashmap-lookup-hit/-miss/-iterate,
# hashset-contains and the ImmutableMap/ImmutableSet ids, same sizes, inputs
# and checks as the Cajeta benchmarks. Libraries: dict, set,
# types.MappingProxyType over a dict copy, frozenset.
import os
import sys
from types import MappingProxyType

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "common"))
from collgen import Gen, bench, hex16, key, params, row, selected, size_note, sized  # noqa: E402

W, T = params()
N = 1_000_000
Q = 10_000_000


def run(bench_id, lib, size, f, check, notes=""):
    if not selected(bench_id):
        return
    s, ok = bench(W, T, f, check)
    print(row(bench_id, lib, "cpython", size, W, T, s, ok, notes), flush=True)


# hashmap-int: insert 1e6 int keys key(i) -> i into an empty dict, look each up.
NI = sized(N)
ikeys = [key(i) for i in range(NI)]


def hm_int():
    m = {}
    for i, k in enumerate(ikeys):
        m[k] = i
    s = 0
    for k in ikeys:
        s += m[k]
    return s


run("hashmap-int", "dict", NI, hm_int, lambda s: s == NI * (NI - 1) // 2, size_note(N))

# hashmap-string: 1e6 16-byte str keys; lookups through separately built equal strings.
skeys = [hex16(key(i)) for i in range(NI)]
sprobes = ["".join(list(k)) for k in skeys]


def hm_str():
    m = {}
    for i, k in enumerate(skeys):
        m[k] = i
    s = 0
    for k in sprobes:
        s += m[k]
    return s


run("hashmap-string", "dict", NI, hm_str, lambda s: s == NI * (NI - 1) // 2,
    size_note(N, "lookups through a separate copy of the keys"))
del skeys, sprobes

# hashset-dedup: add 1e7 draws over 1e6 values; distinct count from a seen table.
ND = sized(10 * N)
dvals = Gen().indices(ND, ND // 10)
dexp = len(set(dvals))


def dedup():
    st_ = set()
    add = st_.add
    for v in dvals:
        add(v)
    return len(st_)


run("hashset-dedup", "set", ND, dedup, lambda c: c == dexp, size_note(10 * N))
del dvals

keys = [key(i) for i in range(N)]
d = {k: i for i, k in enumerate(keys)}
hit_idx = Gen().indices(Q, N)
hit_q = [keys[i] for i in hit_idx]
hit_exp = sum(hit_idx)
miss_q = [key(N + i) for i in hit_idx]
half_idx = Gen().indices(Q, 2 * N)
half_q = [key(i) for i in half_idx]
half_exp = sum(1 for i in half_idx if i < N)
del half_idx


def lookup_sum(m):
    def f():
        s = 0
        for k in hit_q:
            s += m[k]
        return s
    return f


run("hashmap-lookup-hit", "dict", Q, lookup_sum(d), lambda s: s == hit_exp)


def miss():
    c = 0
    for k in miss_q:
        if k not in d:
            c += 1
    return c


run("hashmap-lookup-miss", "dict", Q, miss, lambda c: c == Q)


def iterate():
    s = 0
    for _ in range(10):
        for v in d.values():
            s += v
    return s


run("hashmap-iterate", "dict", Q, iterate, lambda s: s == 10 * (N * (N - 1) // 2), "for v in d.values()")

st = set(keys)


def contains(sset):
    def f():
        c = 0
        for k in half_q:
            if k in sset:
                c += 1
        return c
    return f


run("hashset-contains", "set", Q, contains(st), lambda c: c == half_exp)

k7 = key(7)


def im_build():
    t = 0
    pr = None
    for _ in range(10):
        m = MappingProxyType(dict(d))
        t += len(m)
        pr = m[k7]
    return t, pr


run("immutablemap-build", "types.MappingProxyType", 10 * N, im_build, lambda r: r == (10 * N, 7),
    "MappingProxyType(dict(d)): the copy is the freeze")
frozen_map = MappingProxyType(dict(d))
run("immutablemap-lookup", "types.MappingProxyType", Q, lookup_sum(frozen_map), lambda s: s == hit_exp)


def is_build():
    t = 0
    pr = False
    for _ in range(10):
        f = frozenset(st)
        t += len(f)
        pr = k7 in f
    return t, pr


run("immutableset-build", "frozenset", 10 * N, is_build, lambda r: r == (10 * N, True))
run("immutableset-contains", "frozenset", Q, contains(frozenset(st)), lambda c: c == half_exp)
