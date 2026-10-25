#!/usr/bin/env python3
# Python collection/ competitors for the Cache ids: functools.lru_cache, same
# sizes, inputs and checks as the Cajeta benchmarks. lru_cache is a memoizing
# decorator, so a "get" is a call of the cached function: a hit returns the
# memo, a miss runs the function (identity) and inserts. cache-miss and
# cache-evict are therefore the same calls (it has no insert without a get);
# it has no TTL, so cache-ttl is skipped (the page names no Python competitor
# for it).
import functools
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "common"))
from collgen import Gen, bench, key, params, row, selected, skip  # noqa: E402

W, T = params()
C = 100_000
Q = 10_000_000
LIB = "functools.lru_cache"


def run(bench_id, size, f, check, notes=""):
    if not selected(bench_id):
        return
    s, ok = bench(W, T, f, check)
    print(row(bench_id, LIB, "cpython", size, W, T, s, ok, notes), flush=True)


def ident(k):
    return k


hot = functools.lru_cache(maxsize=C)(ident)
for i in range(C):
    hot(key(i))
hit_q = [key(i) for i in Gen().indices(Q, C)]


def hits():
    h0 = hot.cache_info().hits
    for k in hit_q:
        hot(k)
    return hot.cache_info().hits - h0


run("cache-hit", Q, hits, lambda h: h == Q)

keys = [key(i) for i in range(Q)]


def miss():
    f = functools.lru_cache(maxsize=C)(ident)
    for k in keys:
        f(k)
    ci = f.cache_info()
    return ci.misses, ci.currsize


run("cache-miss", Q, miss, lambda r: r == (Q, C), "a call that misses runs the function and inserts")
run("cache-evict", Q, miss, lambda r: r == (Q, C), "lru_cache has no insert without a get: the cache-miss calls")
if selected("cache-ttl"):
    print(skip("cache-ttl", LIB, 1_000_000, "functools.lru_cache has no TTL (the page names Caffeine moka and ristretto)"))
