#!/usr/bin/env python3
"""Generate the nucleo/table-* inputs (bench/nucleo/Table.md): a port of the
h2oai db-benchmark data generators (groupby-datagen.R, join-datagen.R) with a
recorded seed, plus the answers DuckDB computes once over the same files.

    gen_dbbench.py <dir> <N> [<N> ...]       N in 1e5, 1e6, 1e7

Writes, under <dir>/dbbench/ (one set per N; an existing set whose .meta
matches this generator's version and seed is reused):

  G1_<N>_1e2_0_0.csv      groupby table: id1 id2 id3 (strings) id4 id5 id6
                          (ints) v1 v2 (ints) v3 (float, 6 decimals)
  J1_<N>_NA_0_0.csv       join left table x: id1 id2 id3 (ints) id4 id5 id6
                          (strings) v1 (float)
  J1_<N>_small_0_0.csv    join right tables (db-benchmark's N/1e6, N/1e3, N
  J1_<N>_medium_0_0.csv   rows; see SMALL below)
  J1_<N>_big_0_0.csv
  answers_<N>.csv         question,rows,c1,c2,c3: the checksums DuckDB computes
                          (unused checksum columns are 0.0)
  dbbench_<N>.meta        generator version, seed, row counts, sha256 of each csv

The format is CSV (RFC 4180, header row, no quoting needed: no field holds a
comma), because every competitor reads CSV natively (pandas.read_csv,
polars.read_csv, DuckDB read_csv) and the Cajeta side binds it with
cajeta.codec.csv; reading is outside every timed region.

The port. db-benchmark generates with R's set.seed(108) and sample()/runif();
R's generator cannot be reproduced bit for bit outside R, so this port keeps
the seed (108), the column definitions, the cardinalities and the key-overlap
structure, drawing from numpy's PCG64 (default_rng(108)) instead:

  groupby (K = 100, no NAs, unsorted):
    id1, id2 = sample("id%03d" % 1..K)          id3 = sample("id%010d" % 1..N/K)
    id4, id5 = sample(1..K)                     id6 = sample(1..N/K)
    v1 = sample(1..5)  v2 = sample(1..15)       v3 = round(runif(0, 100), 6)

  join: split_xlr(n) permutes 1..1.1n into x-only (0.9n), both (0.1n) and
  right-only (0.1n) keys; x draws its keys from x-only + both, each right table
  from x-only + right-only, so ~90% of x matches. id4..id6 are "id%d" of
  id1..id3. sample_all(keys, size) = every key once, then draws with
  replacement up to size, shuffled (R's sample_all).

  SMALL: db-benchmark's small table has N/1e6 rows, which is 1 row at 1e6 and
  leaves split_xlr no matching key at all; the port floors it at 10 rows (9
  matching keys) at every N, and says so in the meta.

The questions (db-benchmark's groupby q1-q5 and join q1-q5), answered by DuckDB:
  groupby q1  sum v1 by id1                 q2  sum v1 by id1, id2
          q3  sum v1, mean v3 by id3        q4  mean v1, v2, v3 by id4
          q5  sum v1, v2, v3 by id6
  join    q1  small inner on int id1        q2  medium inner on int id2
          q3  medium left on int id2        q4  medium inner on string id5
          q5  big inner on int id3
  sort    order by id4 asc, v3 desc over G1 (rows, first and last v3)
  filter  v3 < 10.0 over G1 (selects ~10%; rows and sum v1)
Each answer row: question, result rows, then the sum of every aggregate (or
v1, v2) column of the result, the db-benchmark `chk` convention.
"""
import hashlib
import os
import sys

VERSION = "dbbench-port-1"
SEED = 108
K = 100
SMALL_FLOOR = 10


def parse_n(s):
    return int(float(s))


def label(n):
    e = len(str(n)) - 1
    if n != 10 ** e:
        raise SystemExit(f"gen_dbbench: N must be a power of ten, got {n}")
    return f"1e{e}"


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 22), b""):
            h.update(b)
    return h.hexdigest()


def gen_groupby(np, pl, n, path):
    rng = np.random.default_rng(SEED)
    nk = max(n // K, 1)

    def ids(vals, width):
        return "id" + pl.Series(vals).cast(pl.Utf8).str.zfill(width)

    df = pl.DataFrame({
        "id1": ids(rng.integers(1, K + 1, n), 3),
        "id2": ids(rng.integers(1, K + 1, n), 3),
        "id3": ids(rng.integers(1, nk + 1, n), 10),
        "id4": rng.integers(1, K + 1, n),
        "id5": rng.integers(1, K + 1, n),
        "id6": rng.integers(1, nk + 1, n),
        "v1": rng.integers(1, 6, n),
        "v2": rng.integers(1, 16, n),
        "v3": np.round(rng.uniform(0.0, 100.0, n), 6),
    })
    df.write_csv(path, float_precision=6)


def gen_join(np, pl, n, paths):
    rng = np.random.default_rng(SEED)

    def split_xlr(m):
        key = rng.permutation(np.arange(1, int(m * 1.1) + 1, dtype=np.int64))
        a, b = int(m * 0.9), m
        return key[:a], key[a:b], key[b:int(m * 1.1)]

    def sample_all(keys, size):
        extra = rng.choice(keys, size - len(keys), replace=True) if size > len(keys) else keys[:0]
        return rng.permutation(np.concatenate([keys, extra]))

    def sample(keys, size):
        return rng.choice(keys, size, replace=False)

    n_small = max(n // 1000000, SMALL_FLOOR)
    n_med = n // 1000
    k1 = split_xlr(n_small)
    k2 = split_xlr(n_med)
    k3 = split_xlr(n)

    def strs(a):
        return "id" + pl.Series(a).cast(pl.Utf8)

    def v(m):
        return np.round(rng.uniform(0.0, 100.0, m), 6)

    x1 = sample_all(np.concatenate([k1[0], k1[1]]), n)
    x2 = sample_all(np.concatenate([k2[0], k2[1]]), n)
    x3 = sample_all(np.concatenate([k3[0], k3[1]]), n)
    x = pl.DataFrame({"id1": x1, "id2": x2, "id3": x3,
                      "id4": strs(x1), "id5": strs(x2), "id6": strs(x3), "v1": v(n)})
    x.write_csv(paths["x"], float_precision=6)
    del x, x1, x2, x3

    s1 = sample(np.concatenate([k1[0], k1[2]]), n_small)
    small = pl.DataFrame({"id1": s1, "id4": strs(s1), "v2": v(n_small)})
    small.write_csv(paths["small"], float_precision=6)

    m1 = sample_all(np.concatenate([k1[0], k1[2]]), n_med)
    m2 = sample(np.concatenate([k2[0], k2[2]]), n_med)
    med = pl.DataFrame({"id1": m1, "id2": m2, "id4": strs(m1), "id5": strs(m2), "v2": v(n_med)})
    med.write_csv(paths["medium"], float_precision=6)

    b1 = sample_all(np.concatenate([k1[0], k1[2]]), n)
    b2 = sample_all(np.concatenate([k2[0], k2[2]]), n)
    b3 = sample(np.concatenate([k3[0], k3[2]]), n)
    big = pl.DataFrame({"id1": b1, "id2": b2, "id3": b3,
                        "id4": strs(b1), "id5": strs(b2), "id6": strs(b3), "v2": v(n)})
    big.write_csv(paths["big"], float_precision=6)
    return n_small, n_med


# The answers. Every competitor (and the Cajeta side) checks its result
# against these lines.
GROUPBY_SQL = {
    "groupby-q1": "SELECT id1, sum(v1) AS v1 FROM g GROUP BY id1",
    "groupby-q2": "SELECT id1, id2, sum(v1) AS v1 FROM g GROUP BY id1, id2",
    "groupby-q3": "SELECT id3, sum(v1) AS v1, avg(v3) AS v3 FROM g GROUP BY id3",
    "groupby-q4": "SELECT id4, avg(v1) AS v1, avg(v2) AS v2, avg(v3) AS v3 FROM g GROUP BY id4",
    "groupby-q5": "SELECT id6, sum(v1) AS v1, sum(v2) AS v2, sum(v3) AS v3 FROM g GROUP BY id6",
}
GROUPBY_CHK = {"groupby-q1": ["v1"], "groupby-q2": ["v1"], "groupby-q3": ["v1", "v3"],
               "groupby-q4": ["v1", "v2", "v3"], "groupby-q5": ["v1", "v2", "v3"]}
JOIN_SQL = {
    "join-q1": "SELECT x.*, s.id4 AS r_id4, s.v2 FROM x JOIN s USING (id1)",
    "join-q2": "SELECT x.*, m.v2 FROM x JOIN m USING (id2)",
    "join-q3": "SELECT x.*, m.v2 FROM x LEFT JOIN m USING (id2)",
    "join-q4": "SELECT x.*, m.v2 FROM x JOIN m USING (id5)",
    "join-q5": "SELECT x.*, b.v2 FROM x JOIN b USING (id3)",
}


def answers(duckdb, d, lab, out):
    con = duckdb.connect()
    con.execute("SET threads TO 4")
    g = os.path.join(d, f"G1_{lab}_1e2_0_0.csv")
    con.execute(f"CREATE TABLE g AS SELECT * FROM read_csv('{g}', header=true)")
    for nm, f in (("x", "NA"), ("s", "small"), ("m", "medium"), ("b", "big")):
        p = os.path.join(d, f"J1_{lab}_{f}_0_0.csv")
        con.execute(f"CREATE TABLE {nm} AS SELECT * FROM read_csv('{p}', header=true)")
    lines = []
    for q, sql in GROUPBY_SQL.items():
        cols = GROUPBY_CHK[q]
        agg = ", ".join(f"sum({c})" for c in cols)
        r = con.execute(f"SELECT count(*), {agg} FROM ({sql})").fetchone()
        lines.append([q] + [str(r[0])] + [repr(float(x)) for x in r[1:]])
    for q, sql in JOIN_SQL.items():
        r = con.execute(f"SELECT count(*), sum(v1), sum(v2) FROM ({sql})").fetchone()
        lines.append([q, str(r[0]), repr(float(r[1])), repr(float(r[2]))])
    r = con.execute("SELECT count(*) FROM g").fetchone()
    first = con.execute("SELECT id4, v3 FROM g ORDER BY id4 ASC, v3 DESC LIMIT 1").fetchone()
    last = con.execute("SELECT id4, v3 FROM g ORDER BY id4 DESC, v3 ASC LIMIT 1").fetchone()
    lines.append(["sort", str(r[0]), repr(float(first[1])), repr(float(last[1]))])
    r = con.execute("SELECT count(*), sum(v1) FROM g WHERE v3 < 10.0").fetchone()
    lines.append(["filter", str(r[0]), repr(float(r[1]))])
    with open(out, "w") as f:
        f.write("question,rows,c1,c2,c3\n")
        for ln in lines:
            f.write(",".join((ln + ["0.0", "0.0", "0.0"])[:5]) + "\n")


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(2)
    import numpy as np
    import polars as pl
    import duckdb
    base = os.path.join(sys.argv[1], "dbbench")
    os.makedirs(base, exist_ok=True)
    for arg in sys.argv[2:]:
        n = parse_n(arg)
        lab = label(n)
        meta = os.path.join(base, f"dbbench_{lab}.meta")
        files = {"g": f"G1_{lab}_1e2_0_0.csv", "x": f"J1_{lab}_NA_0_0.csv",
                 "small": f"J1_{lab}_small_0_0.csv", "medium": f"J1_{lab}_medium_0_0.csv",
                 "big": f"J1_{lab}_big_0_0.csv"}
        ans = os.path.join(base, f"answers_{lab}.csv")
        if os.path.exists(meta) and os.path.exists(ans):
            with open(meta) as f:
                kv = dict(l.rstrip("\n").split("\t", 1) for l in f if "\t" in l)
            if kv.get("version") == VERSION and kv.get("seed") == str(SEED) and all(
                    os.path.exists(os.path.join(base, p)) and kv.get(p) == sha256(os.path.join(base, p))
                    for p in files.values()):
                print(f"[cached]   dbbench {lab} in {base}")
                continue
        print(f"[generate] dbbench {lab} (seed {SEED}) -> {base}", flush=True)
        gen_groupby(np, pl, n, os.path.join(base, files["g"]))
        ns, nm = gen_join(np, pl, n, {k: os.path.join(base, files[k]) for k in ("x", "small", "medium", "big")})
        answers(duckdb, base, lab, ans)
        with open(meta, "w") as f:
            f.write(f"version\t{VERSION}\nseed\t{SEED}\nrows\t{n}\nk\t{K}\n")
            f.write(f"small_rows\t{ns}\t(db-benchmark N/1e6, floored at {SMALL_FLOOR})\nmedium_rows\t{nm}\n")
            f.write("numpy\t%s\npolars\t%s\nduckdb\t%s\n" % (np.__version__, pl.__version__, duckdb.__version__))
            for p in files.values():
                f.write(f"{p}\t{sha256(os.path.join(base, p))}\n")
        print(f"[ok]       dbbench {lab}: answers_{lab}.csv", flush=True)


if __name__ == "__main__":
    main()
