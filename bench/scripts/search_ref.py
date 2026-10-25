#!/usr/bin/env python3
"""Reference data and expected constants for the search/ benchmarks
(bench/search/*.md, benchmark-reference unit 13). Every language generates the
same inputs from this description; this script is the reference that prints
the constants each benchmark checks against.

PRNG (all languages, int64 arithmetic, no overflow):
    x = (x * 1103515245 + 12345) & 0x7fffffff      # LCG31, then
    rnd(m) = ((x >> 15) * m) >> 16                   # top 16 bits, scaled; after the update

distance (search/distance-*), seed 12345, M pairs (1e6; smoke 1e5):
    per pair: a = 16 chars 'a'+rnd(8), then b = 16 chars 'a'+rnd(8)
    levenshtein: sum of Levenshtein distances (int64, exact)
    jaro-winkler: sum of Jaro-Winkler similarities (prefix scale 0.1, prefix
    capped at 4, applied only when jaro > 0.7; match window max(|a|,|b|)/2 - 1;
    half transpositions by integer division, as strsim, rapidfuzz and
    commons-text do), checked within 1e-6 * M

keys (search/fuzzy-topk, search/ngram-*), seed 777, K keys (1e5; smoke 1e4):
    per key: L = 8 + rnd(9); L chars 'a'+rnd(26)
queries, Q of them, seed 999 (fuzzy-topk: Q = 1e4, smoke 1e3; ngram-query:
Q = 1e5, smoke 1e4), over the K keys:
    src = rnd(K); op = rnd(3); key = keys[src], n = len(key)
    op 0 substitute: p = rnd(n), c = 'a'+rnd(26), q = key[:p] + c + key[p+1:]
    op 1 transpose:  p = rnd(n-1), q = key with p and p+1 swapped
    op 2 delete:     p = rnd(n),   q = key[:p] + key[p+1:]
fuzzy-topk: top-10 keys by OSA (optimal string alignment) distance <= t,
    t = max(1, len(q) // 2), ranked by (distance, key bytes); recall@10
    against that brute force over the first 100 queries.
ngram: lowercased trigrams; a key shorter than 3 is one gram (itself).
    postings = sum over keys of the gram count (duplicates kept, as
    cajeta.search.ngram.Index appends one posting per gram occurrence);
    distinct postings = sum over keys of the distinct-gram count (Lucene's
    view: one posting per term per document).
    candidates = sum over queries of the number of distinct keys sharing at
    least one trigram with the query.

Usage: search_ref.py [--smoke] [--json]
"""
import json
import sys


class Lcg:
    def __init__(self, seed):
        self.x = seed

    def rnd(self, m):
        self.x = (self.x * 1103515245 + 12345) & 0x7FFFFFFF
        return ((self.x >> 15) * m) >> 16


def pairs(m):
    g = Lcg(12345)
    out = []
    for _ in range(m):
        a = "".join(chr(97 + g.rnd(8)) for _ in range(16))
        b = "".join(chr(97 + g.rnd(8)) for _ in range(16))
        out.append((a, b))
    return out


def keys(k):
    g = Lcg(777)
    out = []
    for _ in range(k):
        n = 8 + g.rnd(9)
        out.append("".join(chr(97 + g.rnd(26)) for _ in range(n)))
    return out


def queries(ks, q):
    g = Lcg(999)
    out = []
    for _ in range(q):
        key = ks[g.rnd(len(ks))]
        op = g.rnd(3)
        n = len(key)
        if op == 0:
            p = g.rnd(n)
            c = chr(97 + g.rnd(26))
            s = key[:p] + c + key[p + 1:]
        elif op == 1:
            p = g.rnd(n - 1)
            s = key[:p] + key[p + 1] + key[p] + key[p + 2:]
        else:
            p = g.rnd(n)
            s = key[:p] + key[p + 1:]
        out.append(s)
    return out


def grams(s, n=3):
    s = s.lower()
    if not s:
        return []
    if len(s) < n:
        return [s]
    return [s[i:i + n] for i in range(len(s) - n + 1)]


def osa(a, b):
    n, m = len(a), len(b)
    if n == 0:
        return m
    if m == 0:
        return n
    pp = [0] * (m + 1)
    p = list(range(m + 1))
    for i in range(1, n + 1):
        c = [i] + [0] * m
        for k in range(1, m + 1):
            cost = 0 if a[i - 1] == b[k - 1] else 1
            best = min(p[k] + 1, c[k - 1] + 1, p[k - 1] + cost)
            if i > 1 and k > 1 and a[i - 1] == b[k - 2] and a[i - 2] == b[k - 1]:
                best = min(best, pp[k - 2] + 1)
            c[k] = best
        pp, p = p, c
    return p[m]


def main():
    smoke = "--smoke" in sys.argv
    from rapidfuzz.distance import Levenshtein, JaroWinkler, OSA

    m = 100000 if smoke else 1000000
    ps = pairs(m)
    lev = sum(Levenshtein.distance(a, b) for a, b in ps)
    jw = sum(JaroWinkler.similarity(a, b) for a, b in ps)

    k = 10000 if smoke else 100000
    ks = keys(k)
    post = sum(len(grams(s)) for s in ks)
    dpost = sum(len(set(grams(s))) for s in ks)
    index = {}
    for i, s in enumerate(ks):
        for g in grams(s):
            index.setdefault(g, []).append(i)
    nq = 10000 if smoke else 100000
    cand = 0
    for qs in queries(ks, nq):
        seen = set()
        for g in grams(qs):
            seen.update(index.get(g, ()))
        cand += len(seen)

    fq = queries(ks, 100)
    truth_hits = 0
    for qs in fq:
        t = max(1, len(qs) // 2)
        scored = [(OSA.distance(qs, s, score_cutoff=t), s) for s in ks]
        truth_hits += len(sorted(x for x in scored if x[0] <= t)[:10])

    res = {
        "smoke": smoke, "pairs": m, "levenshtein_sum": lev, "jaro_winkler_sum": jw,
        "keys": k, "postings": post, "distinct_postings": dpost,
        "ngram_queries": nq, "candidates": cand, "fuzzy_truth_hits_first100": truth_hits,
    }
    if "--json" in sys.argv:
        print(json.dumps(res))
    else:
        for kk, v in res.items():
            print(f"{kk}\t{v!r}")


if __name__ == "__main__":
    main()
