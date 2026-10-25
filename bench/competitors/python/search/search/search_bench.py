"""search/ competitors in Python (bench/search/*.md): rapidfuzz for distance
and fuzzy top-k, a dict-of-lists trigram index for ngram (the page names no
Python library). Inputs from the shared description (scripts/search_ref.py,
the LCG31 generator); constants are search_ref.py's."""
import importlib.metadata as md
import platform

from rapidfuzz import process
from rapidfuzz.distance import JaroWinkler, Levenshtein, OSA

import _rows as R

SM = R.smoke()
WARM = int(R.env("PROFILE_WARMUP", "1"))
TRIALS = int(R.env("PROFILE_TRIALS", "5"))
RFV = md.version("rapidfuzz")
PYV = platform.python_version()
SMOKE_NOTE = "; smoke size" if SM else ""


class Lcg:
    def __init__(self, seed):
        self.x = seed

    def rnd(self, m):
        self.x = (self.x * 1103515245 + 12345) & 0x7FFFFFFF
        return ((self.x >> 15) * m) >> 16


def pairs(m):
    g = Lcg(12345)
    a, b = [], []
    for _ in range(m):
        a.append("".join(chr(97 + g.rnd(8)) for _ in range(16)))
        b.append("".join(chr(97 + g.rnd(8)) for _ in range(16)))
    return a, b


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


def grams(s):
    s = s.lower()
    if not s:
        return []
    if len(s) < 3:
        return [s]
    return [s[i:i + 3] for i in range(len(s) - 2)]


def tie_recall(results, truth):
    """Hits of `results` (key, dist) against `truth` (sorted (dist, key) list): a
    result counts when it is in truth or ties the last truth distance (a library
    may break distance ties by index rather than by key bytes)."""
    if not truth:
        return 0
    tset = {k for _, k in truth}
    last = truth[-1][0]
    h = 0
    for k, d in results:
        if k in tset or (len(truth) == 10 and d == last):
            h += 1
    return min(h, len(truth))


def main():
    m = 100000 if SM else 1000000
    a, b = pairs(m)

    def lev():
        s = 0
        for x, y in zip(a, b):
            s += Levenshtein.distance(x, y)
        return s

    st, s = R.measure(lev, WARM, TRIALS)
    want = 1276356 if SM else 12760743
    R.row("distance-levenshtein", m, "rapidfuzz", RFV, WARM, TRIALS, st, s == want,
          f"rapidfuzz.distance.Levenshtein.distance per pair; sum {s} == {want}{SMOKE_NOTE} ({m} pairs in place of 1e6)" if SM
          else f"rapidfuzz.distance.Levenshtein.distance per pair; sum {s} == {want}", "pairs/s")

    def jw():
        s = 0.0
        for x, y in zip(a, b):
            s += JaroWinkler.similarity(x, y)
        return s

    st, s = R.measure(jw, WARM, TRIALS)
    want = 61342.531185943684 if SM else 613325.3143160543
    R.row("distance-jaro-winkler", m, "rapidfuzz", RFV, WARM, TRIALS, st, abs(s - want) <= 1e-6 * m,
          f"rapidfuzz.distance.JaroWinkler.similarity (prefix_weight 0.1); sum {s:.6f} within 1e-6*M of {want}"
          + (f"{SMOKE_NOTE} ({m} pairs in place of 1e6)" if SM else ""), "pairs/s")

    k = 10000 if SM else 100000
    ks = keys(k)

    # fuzzy-topk: rapidfuzz.process.extract with the OSA distance (brute force over all keys).
    q = 1000 if SM else 10000
    qs = queries(ks, q)

    def topk():
        tot = 0
        res = []
        for s in qs:
            t = max(1, len(s) // 2)
            r = process.extract(s, ks, scorer=OSA.distance, limit=10, score_cutoff=t)
            tot += len(r)
            res.append(r)
        return tot, res

    st, (tot, res) = R.measure(topk, WARM, TRIALS)
    hits = truth_n = 0
    for i in range(100):
        s = qs[i]
        t = max(1, len(s) // 2)
        truth = sorted((d, kk) for kk in ks if (d := OSA.distance(s, kk, score_cutoff=t)) <= t)[:10]
        truth_n += len(truth)
        hits += tie_recall([(x[0], x[1]) for x in res[i]], truth)
    want_truth = 107 if SM else 178
    recall = hits / truth_n if truth_n else 0.0
    R.row("fuzzy-topk", q, "rapidfuzz", RFV, WARM, TRIALS, st, recall >= 0.99 and truth_n == want_truth,
          f"rapidfuzz.process.extract(scorer=OSA.distance; score_cutoff=t=max(1;len/2); limit=10) brute force over {k} keys; "
          f"recall@10 {recall:.4f} vs brute-force OSA over the first 100 queries (truth hits {truth_n} == {want_truth}; "
          f"distance ties at rank 10 count as hits: rapidfuzz breaks ties by index); {tot} results"
          + (f"{SMOKE_NOTE} ({q} queries over {k} keys in place of 1e4 over 1e5)" if SM else ""), "queries/s")

    # ngram-build: dict gram -> list of ids (duplicates kept, as cajeta.search.ngram.Index does).
    def build():
        idx = {}
        for i, s in enumerate(ks):
            for g in grams(s):
                lst = idx.get(g)
                if lst is None:
                    idx[g] = [i]
                else:
                    lst.append(i)
        return idx

    st, idx = R.measure(build, WARM, TRIALS)
    post = sum(len(v) for v in idx.values())
    want = 100205 if SM else 998746
    R.row("ngram-build", k, "dict (stdlib)", PYV, WARM, TRIALS, st, post == want,
          f"dict of trigram -> list of key ids (std only: the page names no Python library); postings {post} == {want}"
          + (f"{SMOKE_NOTE} ({k} keys in place of 1e5)" if SM else ""), "keys/s")

    nq = 10000 if SM else 100000
    qq = queries(ks, nq)

    def query():
        c = 0
        for s in qq:
            seen = set()
            for g in grams(s):
                lst = idx.get(g)
                if lst is not None:
                    seen.update(lst)
            c += len(seen)
        return c

    st, c = R.measure(query, WARM, TRIALS)
    want = 545614 if SM else 53088630
    R.row("ngram-query", nq, "dict (stdlib)", PYV, WARM, TRIALS, st, c == want,
          f"distinct candidate ids per query via a set over the query's trigram postings; candidates {c} == {want}"
          + (f"{SMOKE_NOTE} ({nq} queries over {k} keys in place of 1e5 over 1e5)" if SM else ""), "queries/s")


if __name__ == "__main__":
    main()
