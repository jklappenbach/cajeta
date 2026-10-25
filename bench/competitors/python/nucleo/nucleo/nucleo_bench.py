#!/usr/bin/env python3
# nucleo/ competitor, Python (benchmark-reference unit 12, spec §6.9): the
# workloads of bench/nucleo/*.md with the libraries those pages name, at the
# sizes and with the checks of the Cajeta side (bench/src/main/cajeta/bench/
# nucleo/), one v1 row (31 columns) per id/variant/library; run.sh maps them to
# v2 with catalog ids nucleo/<slug>.
#
#   Table      pandas, polars, DuckDB over the db-benchmark port's CSVs
#              ($PROFILE_DATA_DIR/dbbench, datasets/gen_dbbench.py, seed 108),
#              checked against answers_<N>.csv (DuckDB, computed once)
#   Column     pyarrow.compute, polars Series, NumPy
#   Tape       PyTorch autograd (CPU), JAX value_and_grad
#   Fuse       NumPy (unfused), numexpr, torch.compile, jax.jit
#   Transforms JAX grad/vmap/jit, torch.func.grad/vmap, torch.compile
#
# Only the workload is timed; loading, references and checks are outside the
# timed region, and every check reads the last trial's output, so nothing
# elides. Engines run with their default threading (pandas and NumPy single-
# threaded; polars, DuckDB, PyTorch and XLA use every core); the Cajeta side is
# single-threaded. The GPU is never touched: CUDA_VISIBLE_DEVICES is emptied
# and JAX is pinned to its CPU platform before either is imported.
# Env: PROFILE_RUN_ID, PROFILE_RUN_TS, PROFILE_WARMUP, PROFILE_TRIALS,
#      PROFILE_LANG_VERSION, PROFILE_DATA_DIR, NUCLEO_IDS (space list of slugs),
#      NUCLEO_LIBS (space list of libraries), CAJETA_BENCH_SMOKE=1.
import math
import os
import resource
import sys
import time
import traceback

os.environ["CUDA_VISIBLE_DEVICES"] = ""
os.environ.setdefault("JAX_PLATFORMS", "cpu")

SMOKE = os.environ.get("CAJETA_BENCH_SMOKE", "") in ("1", "true", "yes")


def env(k, d):
    v = os.environ.get(k)
    return v if v else d


RUN_ID = env("PROFILE_RUN_ID", "local")
TS = env("PROFILE_RUN_TS", "")
PYVER = env("PROFILE_LANG_VERSION", "")
IDS = env("NUCLEO_IDS", "").split()
LIBS = env("NUCLEO_LIBS", "").split()
HERE = os.path.dirname(os.path.abspath(__file__))
BENCH = os.path.abspath(os.path.join(HERE, "..", "..", "..", ".."))
DATA = os.path.join(env("PROFILE_DATA_DIR", os.path.join(BENCH, "datasets", "cache")), "dbbench")


def wanted(slug, lib=None):
    return (not IDS or slug in IDS) and (lib is None or not LIBS or lib in LIBS)


def iters(warmup, trials):
    # The Cajeta benchmark's own counts; PROFILE_* override; smoke is 1 + 3.
    if SMOKE:
        warmup, trials = 1, 3
    return int(env("PROFILE_WARMUP", str(warmup))), int(env("PROFILE_TRIALS", str(trials)))


def clean(s):
    return str(s).replace(",", ";").replace("\n", " ")


def row(slug, size, lib, ver, warmup, trials, st, status, ok, notes, variant=""):
    mn, med, mean, p95 = st
    okc = "" if ok is None else str(ok).lower()
    rss = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
    return (f"1,{RUN_ID},{TS},{slug},nucleo,,{size},,{size},python,{PYVER},{lib},{ver},CPython as shipped,"
            f"{warmup},{trials},{mn},{med},{mean},{p95},,,{rss},"
            f"-1,-1,-1,-1,{status},{okc},{clean(notes)},{variant}")


def stats(samples):
    s = sorted(samples)
    n = len(s)
    return s[0], s[n // 2], sum(s) // n, s[min(n - 1, n * 95 // 100)]


def timed(f, warmup, trials):
    for _ in range(warmup):
        f()
    samples = []
    r = None
    for _ in range(trials):
        t0 = time.perf_counter_ns()
        r = f()
        samples.append(time.perf_counter_ns() - t0)
    return stats(samples), r


def emit(slug, size, lib, ver, wt, st, ok, notes, variant=""):
    print(row(slug, size, lib, ver, wt[0], wt[1], st, "ok" if ok else "invalid", bool(ok), notes, variant), flush=True)


def skip(slug, lib, reason, variant=""):
    print(row(slug, 0, lib, "", 0, 0, (-1, -1, -1, -1), "skipped", None, reason, variant), flush=True)


def fail(slug, lib, e, variant=""):
    traceback.print_exc(file=sys.stderr)
    print(row(slug, 0, lib, "", 0, 0, (-1, -1, -1, -1), "failed", None, f"{type(e).__name__}: {e}", variant), flush=True)


def rel(a, b):
    return abs(a - b) / max(abs(b), 1.0)


import numpy as np  # noqa: E402

NPV = np.__version__
P = 10007


def u(i):
    i = np.asarray(i, dtype=np.int64)
    return ((i * 7919) % P).astype(np.float64) / 10007.0


SMOKE_NOTE = "; smoke: the 1e6 variant reads the 1e5 set" if SMOKE else ""


def libs_present():
    out = {}
    for name in ("pandas", "polars", "duckdb", "pyarrow", "numexpr", "torch", "jax"):
        try:
            m = __import__(name)
            out[name] = getattr(m, "__version__", "")
        except Exception as e:  # recorded as a skip reason per row
            out[name] = None
            print(f"nucleo_bench: {name} unavailable: {e}", file=sys.stderr)
    return out


LIBV = {}


# ---------------- Table (db-benchmark) ----------------

def file_label(size):
    return "1e5" if (SMOKE and size == "1e6") else size


def size_skip(size):
    if size == "1e7":
        if SMOKE:
            return "1e7 variant not run under CAJETA_BENCH_SMOKE=1 (the smoke reads the 1e5 set for the 1e6 variants)"
        try:
            with open("/proc/meminfo") as f:
                kb = next(int(l.split()[1]) for l in f if l.startswith("MemAvailable:"))
            if kb < 32 * 1024 * 1024:
                return f"1e7 variant needs 32 GB available; MemAvailable is {kb // 1048576} GB"
        except Exception:
            pass
    lab = file_label(size)
    if not os.path.exists(os.path.join(DATA, f"answers_{lab}.csv")):
        return f"dbbench {lab} set missing under $PROFILE_DATA_DIR/dbbench (run datasets/gen_dbbench.py <dir> {lab})"
    return None


def answers(size):
    lab = file_label(size)
    out = {}
    with open(os.path.join(DATA, f"answers_{lab}.csv")) as f:
        next(f)
        for line in f:
            q, rows, c1, c2, c3 = line.strip().split(",")
            out[q] = (int(rows), float(c1), float(c2), float(c3))
    return out


def csv_path(size, stem):
    return os.path.join(DATA, stem.replace("<N>", file_label(size)))


G_FILE = "G1_<N>_1e2_0_0.csv"
J_FILES = {"x": "J1_<N>_NA_0_0.csv", "small": "J1_<N>_small_0_0.csv",
           "medium": "J1_<N>_medium_0_0.csv", "big": "J1_<N>_big_0_0.csv"}
GB_CHK = {"q1": ["v1"], "q2": ["v1"], "q3": ["v1", "v3"], "q4": ["v1", "v2", "v3"], "q5": ["v1", "v2", "v3"]}
SIZES = ["1e6", "1e7"]
QS = ["q1", "q2", "q3", "q4", "q5"]
TABLE_LIBS = ["pandas", "polars", "duckdb"]
TABLE_NOTE = "strings stay strings (no categorical), as on the Cajeta side; engine default threading"


def gb_check(ans, q, nrows, sums):
    r = ans[f"groupby-{q}"]
    if nrows != r[0]:
        return False
    return all(rel(s, r[1 + k]) <= 1e-9 for k, s in enumerate(sums))


class Engine:
    """One library's load + groupby/join/sort/filter, results checked by the caller."""

    def __init__(self, lib):
        self.lib = lib

    def load_g(self, size):
        p = csv_path(size, G_FILE)
        if self.lib == "pandas":
            import pandas as pd
            return pd.read_csv(p)
        if self.lib == "polars":
            import polars as pl
            return pl.read_csv(p)
        import duckdb
        con = duckdb.connect()
        con.execute(f"CREATE TABLE g AS SELECT * FROM read_csv('{p}', header=true)")
        return con

    def load_j(self, size):
        if self.lib == "pandas":
            import pandas as pd
            return {k: pd.read_csv(csv_path(size, f)) for k, f in J_FILES.items()}
        if self.lib == "polars":
            import polars as pl
            return {k: pl.read_csv(csv_path(size, f)) for k, f in J_FILES.items()}
        import duckdb
        con = duckdb.connect()
        for k, f in J_FILES.items():
            con.execute(f"CREATE TABLE {k} AS SELECT * FROM read_csv('{csv_path(size, f)}', header=true)")
        return {"con": con}

    # ---- groupby: returns a function computing the result, and a checker
    def groupby(self, g, q):
        lib = self.lib
        if lib == "pandas":
            spec = {"q1": (["id1"], {"v1": "sum"}), "q2": (["id1", "id2"], {"v1": "sum"}),
                    "q3": (["id3"], {"v1": "sum", "v3": "mean"}),
                    "q4": (["id4"], {"v1": "mean", "v2": "mean", "v3": "mean"}),
                    "q5": (["id6"], {"v1": "sum", "v2": "sum", "v3": "sum"})}[q]
            run = lambda: g.groupby(spec[0], as_index=False, sort=False, observed=True, dropna=False).agg(spec[1])
            res = lambda r: (len(r), [float(r[c].sum()) for c in GB_CHK[q]])
            return run, res
        if lib == "polars":
            import polars as pl
            spec = {"q1": (["id1"], [pl.col("v1").sum()]), "q2": (["id1", "id2"], [pl.col("v1").sum()]),
                    "q3": (["id3"], [pl.col("v1").sum(), pl.col("v3").mean()]),
                    "q4": (["id4"], [pl.col("v1").mean(), pl.col("v2").mean(), pl.col("v3").mean()]),
                    "q5": (["id6"], [pl.col("v1").sum(), pl.col("v2").sum(), pl.col("v3").sum()])}[q]
            run = lambda: g.group_by(spec[0]).agg(spec[1])
            res = lambda r: (r.height, [float(r[c].sum()) for c in GB_CHK[q]])
            return run, res
        sql = {"q1": "SELECT id1, sum(v1) AS v1 FROM g GROUP BY id1",
               "q2": "SELECT id1, id2, sum(v1) AS v1 FROM g GROUP BY id1, id2",
               "q3": "SELECT id3, sum(v1) AS v1, avg(v3) AS v3 FROM g GROUP BY id3",
               "q4": "SELECT id4, avg(v1) AS v1, avg(v2) AS v2, avg(v3) AS v3 FROM g GROUP BY id4",
               "q5": "SELECT id6, sum(v1) AS v1, sum(v2) AS v2, sum(v3) AS v3 FROM g GROUP BY id6"}[q]
        run = lambda: g.execute(f"CREATE OR REPLACE TABLE ans AS {sql}")
        cols = ", ".join(f"sum({c})" for c in GB_CHK[q])

        def res(_):
            r = g.execute(f"SELECT count(*), {cols} FROM ans").fetchone()
            return r[0], [float(x) for x in r[1:]]
        return run, res

    # ---- join
    JOIN = {"q1": ("small", "id1", "inner"), "q2": ("medium", "id2", "inner"), "q3": ("medium", "id2", "left"),
            "q4": ("medium", "id5", "inner"), "q5": ("big", "id3", "inner")}

    def join(self, t, q):
        right, key, how = self.JOIN[q]
        lib = self.lib
        if lib == "pandas":
            x, y = t["x"], t[right]
            if q == "q3":
                y = y[["id2", "v2"]]
            run = lambda: x.merge(y, how=how, on=key)
            res = lambda r: (len(r), float(r["v1"].sum()), float(r["v2"].sum()))
            return run, res
        if lib == "polars":
            import polars as pl
            x, y = t["x"], t[right]
            if q == "q3":
                y = y.select(["id2", "v2"])
            run = lambda: x.join(y, on=key, how=how)
            res = lambda r: (r.height, float(r["v1"].sum()), float(r["v2"].sum()))
            return run, res
        con = t["con"]
        rcols = [c for c, in con.execute(f"SELECT column_name FROM (DESCRIBE {right})").fetchall()]
        if q == "q3":
            rcols = ["id2", "v2"]
        xcols = [c for c, in con.execute("SELECT column_name FROM (DESCRIBE x)").fetchall()]
        sel = ", ".join(["x.*"] + [f"y.{c} AS {'r_' + c if c in xcols else c}" for c in rcols if c != key])
        jt = "LEFT JOIN" if how == "left" else "JOIN"
        sql = f"CREATE OR REPLACE TABLE ans AS SELECT {sel} FROM x {jt} {right} y ON x.{key} = y.{key}"
        run = lambda: con.execute(sql)

        def res(_):
            r = con.execute("SELECT count(*), sum(v1), sum(v2) FROM ans").fetchone()
            return r[0], float(r[1]), float(r[2])
        return run, res

    # ---- sort
    def sort(self, g):
        lib = self.lib
        if lib == "pandas":
            run = lambda: g.sort_values(["id4", "v3"], ascending=[True, False])
            res = lambda r: (r["id4"].to_numpy(), r["v3"].to_numpy())
            return run, res
        if lib == "polars":
            run = lambda: g.sort(["id4", "v3"], descending=[False, True])
            res = lambda r: (r["id4"].to_numpy(), r["v3"].to_numpy())
            return run, res
        run = lambda: g.execute("CREATE OR REPLACE TABLE ans AS SELECT * FROM g ORDER BY id4 ASC, v3 DESC")

        def res(_):
            a = g.execute("SELECT id4, v3 FROM ans").fetchnumpy()
            return np.asarray(a["id4"]), np.asarray(a["v3"])
        return run, res

    # ---- filter
    def filter(self, g):
        lib = self.lib
        if lib == "pandas":
            run = lambda: g[g["v3"] < 10.0]
            res = lambda r: (len(r), float(r["v1"].sum()), bool((r["v3"] < 10.0).all()))
            return run, res
        if lib == "polars":
            import polars as pl
            run = lambda: g.filter(pl.col("v3") < 10.0)
            res = lambda r: (r.height, float(r["v1"].sum()), bool((r["v3"] < 10.0).all()))
            return run, res
        run = lambda: g.execute("CREATE OR REPLACE TABLE ans AS SELECT * FROM g WHERE v3 < 10.0")

        def res(_):
            r = g.execute("SELECT count(*), sum(v1), bool_and(v3 < 10.0) FROM ans").fetchone()
            return r[0], float(r[1]), bool(r[2])
        return run, res


def b_table_groupby_join():
    for slug in ("table-groupby", "table-join"):
        for lib in TABLE_LIBS:
            if not wanted(slug, lib):
                continue
            if LIBV.get(lib) is None:
                for size in SIZES:
                    for q in QS:
                        skip(slug, lib, f"{lib} not installed in the venv (run competitors/setup.sh)", f"{q}-{size}")
                continue
            eng = Engine(lib)
            for size in SIZES:
                why = size_skip(size)
                if why:
                    for q in QS:
                        skip(slug, lib, why, f"{q}-{size}")
                    continue
                try:
                    ans = answers(size)
                    data = eng.load_g(size) if slug == "table-groupby" else eng.load_j(size)
                except Exception as e:
                    for q in QS:
                        fail(slug, lib, e, f"{q}-{size}")
                    continue
                for q in QS:
                    v = f"{q}-{size}"
                    try:
                        wt = iters(1, 5)
                        if slug == "table-groupby":
                            run, res = eng.groupby(data, q)
                            st, r = timed(run, *wt)
                            nrows, sums = res(r)
                            ok = gb_check(ans, q, nrows, sums)
                            a = ans[f"groupby-{q}"]
                            nin = int(float(file_label(size)))
                            emit(slug, nin, lib, LIBV[lib], wt, st, ok,
                                 f"db-benchmark groupby-{q} (gen_dbbench.py seed 108); {TABLE_NOTE}; check: rows={a[0]} and aggregate column sums within 1e-9 of DuckDB's answers{SMOKE_NOTE}", v)
                        else:
                            run, res = eng.join(data, q)
                            st, r = timed(run, *wt)
                            nrows, s1, s2 = res(r)
                            a = ans[f"join-{q}"]
                            ok = nrows == a[0] and rel(s1, a[1]) <= 1e-9 and rel(s2, a[2]) <= 1e-9
                            q3 = "; right side projected to (id2 v2) before the left join as on the Cajeta side" if q == "q3" else ""
                            nin = int(float(file_label(size)))
                            emit(slug, nin, lib, LIBV[lib], wt, st, ok,
                                 f"db-benchmark join-{q} (gen_dbbench.py seed 108; small table floored at 10 rows){q3}; {TABLE_NOTE}; check: rows={a[0]} and sum(v1) sum(v2) within 1e-9 of DuckDB's answers{SMOKE_NOTE}", v)
                    except Exception as e:
                        fail(slug, lib, e, v)
                del data


def b_table_sort_filter():
    size = "1e6" if SMOKE else "1e7"
    for slug in ("table-sort", "table-filter"):
        for lib in TABLE_LIBS:
            if not wanted(slug, lib):
                continue
            if LIBV.get(lib) is None:
                skip(slug, lib, f"{lib} not installed in the venv (run competitors/setup.sh)")
                continue
            why = size_skip(size)
            if why:
                skip(slug, lib, why.replace("1e7 variant", slug + " at 1e7"))
                continue
            try:
                eng = Engine(lib)
                ans = answers(size)
                g = eng.load_g(size)
                wt = iters(1, 5)
                nin = int(float(file_label(size)))
                sn = "; smoke: reads the 1e5 set" if SMOKE else ""
                if slug == "table-sort":
                    run, res = eng.sort(g)
                    st, r = timed(run, *wt)
                    id4, v3 = res(r)
                    a = ans["sort"]
                    d4 = np.diff(id4)
                    ordered = bool((d4 >= 0).all() and ((d4 != 0) | (np.diff(v3) <= 0)).all())
                    ok = (len(id4) == a[0] and ordered and float(v3[0]) == a[1] and float(v3[-1]) == a[2])
                    emit(slug, nin, lib, LIBV[lib], wt, st, ok,
                         f"db-benchmark groupby table (gen_dbbench.py seed 108) sorted by id4 asc then v3 desc; {TABLE_NOTE}; check: ordered + first/last v3 equal DuckDB's{sn}")
                else:
                    run, res = eng.filter(g)
                    st, r = timed(run, *wt)
                    n, s1, allok = res(r)
                    a = ans["filter"]
                    ok = n == a[0] and s1 == a[1] and allok
                    emit(slug, nin, lib, LIBV[lib], wt, st, ok,
                         f"db-benchmark groupby table (gen_dbbench.py seed 108) filtered on v3 < 10.0 (~10%); {TABLE_NOTE}; check: rows and sum(v1) equal DuckDB's{sn}")
                del g
            except Exception as e:
                fail(slug, lib, e)


# ---------------- Column ----------------

def b_column_filter():
    slug = "column-filter"
    n = 1000000 if SMOKE else 100000000
    i = np.arange(n, dtype=np.int64)
    vals = (i * 7919) % P
    keep = vals < 5004
    ref_n, ref_s = int(keep.sum()), int(vals[keep].sum())
    sn = "; smoke size n=1e6 in place of 1e8" if SMOKE else ""
    base = f"filter by a precomputed boolean mask (~50%); check: count={ref_n} and sum of kept values"
    wt = iters(1, 5)
    if wanted(slug, "pyarrow"):
        if LIBV.get("pyarrow") is None:
            skip(slug, "pyarrow", "pyarrow not installed in the venv (run competitors/setup.sh)")
        else:
            try:
                import pyarrow as pa
                import pyarrow.compute as pc
                av, am = pa.array(vals), pa.array(keep)    # BooleanArray: bit-packed, as the Cajeta mask
                st, r = timed(lambda: pc.filter(av, am), *wt)
                ok = len(r) == ref_n and int(pc.sum(r).as_py()) == ref_s
                emit(slug, n, "pyarrow", LIBV["pyarrow"], wt, st, ok, "pyarrow.compute.filter(Int64Array, BooleanArray); " + base + sn)
            except Exception as e:
                fail(slug, "pyarrow", e)
    if wanted(slug, "polars"):
        try:
            import polars as pl
            sv, sm = pl.Series(vals), pl.Series(keep)
            st, r = timed(lambda: sv.filter(sm), *wt)
            ok = r.len() == ref_n and int(r.sum()) == ref_s
            emit(slug, n, "polars", LIBV["polars"], wt, st, ok, "polars Series.filter(mask); " + base + sn)
        except Exception as e:
            fail(slug, "polars", e)
    if wanted(slug, "numpy"):
        st, r = timed(lambda: vals[keep], *wt)
        ok = len(r) == ref_n and int(r.sum()) == ref_s
        emit(slug, n, "numpy", NPV, wt, st, ok, "NumPy boolean indexing; " + base + sn)


def b_column_arith():
    slug = "column-arith"
    n = 1000000 if SMOKE else 100000000
    i = np.arange(n, dtype=np.int64)
    a, b, c = u(i), u(i + 1), u(i + 2)
    va, vb, vc = (i % 10) != 0, (i % 13) != 0, (i % 17) != 0
    valid = va & vb & vc
    ref_n, ref_s = int(valid.sum()), float((a + b * c)[valid].sum())
    sn = "; smoke size n=1e6 in place of 1e8" if SMOKE else ""
    base = f"r = a + b * c float64 with nulls (null-propagating); check: valid={ref_n} and sum within 1e-12"
    wt = iters(1, 5)
    if wanted(slug, "pyarrow"):
        if LIBV.get("pyarrow") is None:
            skip(slug, "pyarrow", "pyarrow not installed in the venv (run competitors/setup.sh)")
        else:
            try:
                import pyarrow as pa
                import pyarrow.compute as pc
                pa_a, pa_b, pa_c = pa.array(a, mask=~va), pa.array(b, mask=~vb), pa.array(c, mask=~vc)
                st, r = timed(lambda: pc.add(pa_a, pc.multiply(pa_b, pa_c)), *wt)
                ok = len(r) - r.null_count == ref_n and rel(float(pc.sum(r).as_py()), ref_s) <= 1e-12
                emit(slug, n, "pyarrow", LIBV["pyarrow"], wt, st, ok, "pyarrow.compute add(a; multiply(b; c)); " + base + sn)
            except Exception as e:
                fail(slug, "pyarrow", e)
    if wanted(slug, "polars"):
        try:
            import polars as pl
            sa = pl.Series(a).set(pl.Series(~va), None)
            sb = pl.Series(b).set(pl.Series(~vb), None)
            sc = pl.Series(c).set(pl.Series(~vc), None)
            st, r = timed(lambda: sa + sb * sc, *wt)
            ok = r.len() - r.null_count() == ref_n and rel(float(r.sum()), ref_s) <= 1e-12
            emit(slug, n, "polars", LIBV["polars"], wt, st, ok, "polars Series a + b * c; " + base + sn)
        except Exception as e:
            fail(slug, "polars", e)
    if wanted(slug, "numpy"):
        st, r = timed(lambda: (a + b * c, va & vb & vc), *wt)
        vals, ok_mask = r
        ok = int(ok_mask.sum()) == ref_n and rel(float(vals[ok_mask].sum()), ref_s) <= 1e-12
        emit(slug, n, "numpy", NPV, wt, st, ok, "NumPy values a + b * c plus the validity masks ANDed; " + base + sn)


# ---------------- Tape ----------------

B_, IN_, H_ = 16, 16, 16
LR = 0.1


def mlp_init():
    x = (u(np.arange(B_ * IN_)) - 0.5).astype(np.float32).reshape(B_, IN_)
    y = u(1000 + np.arange(B_)).astype(np.float32)
    w1 = ((u(5000 + np.arange(IN_ * H_)) - 0.5) * 0.5).astype(np.float32).reshape(IN_, H_)
    w2 = ((u(7000 + np.arange(H_ * H_)) - 0.5) * 0.5).astype(np.float32).reshape(H_, H_)
    w3 = ((u(9000 + np.arange(H_)) - 0.5) * 0.5).astype(np.float32)
    return x, y, w1, np.zeros(H_, np.float32), w2, np.zeros(H_, np.float32), w3, np.float32(0.0)


def mlp_reference(steps):
    """float64 hand backprop of the same SGD trajectory: (last loss, sum |dW1|)."""
    x, y, w1, b1, w2, b2, w3, b3 = [np.asarray(t, np.float64) for t in mlp_init()]
    loss = g1s = 0.0
    for _ in range(steps):
        h1 = 1 / (1 + np.exp(-(x @ w1 + b1)))
        h2 = 1 / (1 + np.exp(-(h1 @ w2 + b2)))
        out = h2 @ w3 + b3
        d = out - y
        loss = float((d * d).mean())
        dout = 2 * d / B_
        g3, gb3 = h2.T @ dout, dout.sum()
        d2 = np.outer(dout, w3) * h2 * (1 - h2)
        g2, gb2 = h1.T @ d2, d2.sum(0)
        d1 = (d2 @ w2.T) * h1 * (1 - h1)
        g1, gb1 = x.T @ d1, d1.sum(0)
        g1s = float(np.abs(g1).sum())
        w1, w2, w3 = w1 - LR * g1, w2 - LR * g2, w3 - LR * g3
        b1, b2, b3 = b1 - LR * gb1, b2 - LR * gb2, b3 - LR * gb3
    return loss, g1s


def b_tape():
    slug = "tape-forward-backward"
    steps = 50 if SMOKE else 1000
    ref_loss, ref_g1 = mlp_reference(steps)
    wt = iters(1, 5)
    sn = "; smoke 50 steps in place of 1e3" if SMOKE else ""
    base = (f"MLP batch 16 x 16-16-16-1 sigmoid + MSE + SGD lr 0.1 x {steps} steps (the Cajeta scalar Tape's shape; the page's 256 x 784 is not run); "
            f"check: last loss and sum|dW1| within 1e-3 of a float64 hand backprop")

    def ok_of(loss, g1):
        return abs(loss - ref_loss) / ref_loss <= 1e-3 and abs(g1 - ref_g1) / ref_g1 <= 1e-3

    if wanted(slug, "torch"):
        if LIBV.get("torch") is None:
            skip(slug, "torch", "torch not installed in the venv (run competitors/setup.sh)")
        else:
            try:
                import torch

                def run():
                    x, y, *ps = [torch.tensor(np.asarray(t)) for t in mlp_init()]
                    for p in ps:
                        p.requires_grad_(True)
                    w1, b1, w2, b2, w3, b3 = ps
                    loss = g1 = None
                    for _ in range(steps):
                        h1 = torch.sigmoid(x @ w1 + b1)
                        h2 = torch.sigmoid(h1 @ w2 + b2)
                        out = h2 @ w3 + b3
                        loss = ((out - y) ** 2).mean()
                        loss.backward()
                        with torch.no_grad():
                            g1 = float(w1.grad.abs().sum())
                            for p in ps:
                                p -= LR * p.grad
                                p.grad = None
                    return float(loss.detach()), g1
                st, r = timed(run, *wt)
                emit(slug, steps, "torch", LIBV["torch"], wt, st, ok_of(*r),
                     f"PyTorch autograd (CPU; {torch.get_num_threads()} threads) loss.backward() per step; " + base + sn)
            except Exception as e:
                fail(slug, "torch", e)
    if wanted(slug, "jax"):
        if LIBV.get("jax") is None:
            skip(slug, "jax", "jax not installed in the venv (run competitors/setup.sh)")
        else:
            try:
                import jax
                import jax.numpy as jnp

                def loss_fn(ps, x, y):
                    w1, b1, w2, b2, w3, b3 = ps
                    h1 = jax.nn.sigmoid(x @ w1 + b1)
                    h2 = jax.nn.sigmoid(h1 @ w2 + b2)
                    out = h2 @ w3 + b3
                    return jnp.mean((out - y) ** 2)

                @jax.jit
                def step(ps, x, y):
                    loss, g = jax.value_and_grad(loss_fn)(ps, x, y)
                    return [p - LR * q for p, q in zip(ps, g)], loss, jnp.sum(jnp.abs(g[0]))

                def run():
                    x, y, *ps = [jnp.asarray(t) for t in mlp_init()]
                    loss = g1 = None
                    for _ in range(steps):
                        ps, loss, g1 = step(ps, x, y)
                    return float(loss), float(g1)
                st, r = timed(run, *wt)
                emit(slug, steps, "jax", LIBV["jax"], wt, st, ok_of(*r),
                     "JAX jit(value_and_grad) per step (CPU); " + base + sn)
            except Exception as e:
                fail(slug, "jax", e)


# ---------------- Fuse ----------------

def b_fuse():
    slug = "fuse-elementwise-chain"
    n = 1000000 if SMOKE else 10000000
    a = (u(np.arange(n)) + 0.5).astype(np.float32)
    ad = a.astype(np.float64)
    refv = (ad * 1.5 + 0.25) / 2.0 - 0.125
    ref = float(refv.sum())
    sn = "; smoke size n=1e6 in place of 1e7" if SMOKE else ""
    base = "(a*b + c)/d - e float32 with b=1.5 c=0.25 d=2 e=0.125 scalar operands (the Cajeta Fuse v1 shape); check: sum within 1e-5"
    wt = iters(2, 10)
    f32 = np.float32
    b, c, d, e = f32(1.5), f32(0.25), f32(2.0), f32(0.125)

    def check(r):
        r = np.asarray(r)
        return (r.shape == (n,) and rel(float(r.astype(np.float64).sum()), ref) <= 1e-5
                and abs(float(r[0]) - refv[0]) <= 1e-6 and abs(float(r[-1]) - refv[-1]) <= 1e-6)

    if wanted(slug, "numpy"):
        st, r = timed(lambda: (a * b + c) / d - e, *wt)
        emit(slug, n, "numpy", NPV, wt, st, check(r), "NumPy, one temporary per operator; " + base + sn, "unfused")
    if wanted(slug, "numexpr"):
        if LIBV.get("numexpr") is None:
            skip(slug, "numexpr", "numexpr not installed in the venv (run competitors/setup.sh)", "fused")
        else:
            try:
                import numexpr as ne
                ld = {"a": a, "b": b, "c": c, "d": d, "e": e}
                st, r = timed(lambda: ne.evaluate("(a * b + c) / d - e", local_dict=ld), *wt)
                emit(slug, n, "numexpr", LIBV["numexpr"], wt, st, check(r),
                     f"numexpr.evaluate ({ne.nthreads} threads; result {r.dtype}); " + base + sn, "fused")
            except Exception as ex:
                fail(slug, "numexpr", ex, "fused")
    if wanted(slug, "torch"):
        if LIBV.get("torch") is None:
            skip(slug, "torch", "torch not installed in the venv (run competitors/setup.sh)", "fused")
        else:
            try:
                import torch
                ta = torch.from_numpy(a)
                fn = torch.compile(lambda t: (t * 1.5 + 0.25) / 2.0 - 0.125)
                fn(ta)      # compile outside the timed region (warm)
                st, r = timed(lambda: fn(ta), *wt)
                emit(slug, n, "torch", LIBV["torch"], wt, st, check(r.numpy()),
                     "torch.compile (inductor; CPU; compiled before warmup); " + base + sn, "fused")
            except Exception as ex:
                fail(slug, "torch", ex, "fused")
    if wanted(slug, "jax"):
        if LIBV.get("jax") is None:
            skip(slug, "jax", "jax not installed in the venv (run competitors/setup.sh)", "fused")
        else:
            try:
                import jax
                import jax.numpy as jnp
                ja = jnp.asarray(a)
                fn = jax.jit(lambda t: (t * 1.5 + 0.25) / 2.0 - 0.125)
                fn(ja).block_until_ready()
                st, r = timed(lambda: fn(ja).block_until_ready(), *wt)
                emit(slug, n, "jax", LIBV["jax"], wt, st, check(r),
                     "jax.jit (XLA CPU; compiled before warmup); " + base + sn, "fused")
            except Exception as ex:
                fail(slug, "jax", ex, "fused")


# ---------------- Transforms ----------------

def b_grad():
    slug = "transforms-grad"
    n, reps = (100000, 20) if SMOKE else (1000000, 1000)
    x = (u(np.arange(n)) - 0.5).astype(np.float32)
    xd = x.astype(np.float64)
    ref_v = float((xd * np.exp(xd)).sum())
    ref_g = np.exp(xd) * (1 + xd)
    sn = "; smoke size n=1e5 x 20 in place of 1e6 x 1e3" if SMOKE else ""
    base = f"grad of f(t) = sum(t * exp(t)) over n={n} float32 x {reps} calls; check: value within 1e-4 and every grad element within 1e-5 of exp(t)(1+t)"
    wt = iters(1, 5)

    def ok_of(r):
        v, g = r
        g = np.asarray(g, np.float64)
        return abs(v - ref_v) / abs(ref_v) <= 1e-4 and bool((np.abs(g - ref_g) <= 1e-5 * np.abs(ref_g) + 1e-7).all())

    if wanted(slug, "jax"):
        if LIBV.get("jax") is None:
            skip(slug, "jax", "jax not installed in the venv (run competitors/setup.sh)")
        else:
            try:
                import jax
                import jax.numpy as jnp
                vg = jax.jit(jax.value_and_grad(lambda t: jnp.sum(t * jnp.exp(t))))
                jx = jnp.asarray(x)
                vg(jx)[1].block_until_ready()

                def run():
                    acc = 0.0
                    g = None
                    for _ in range(reps):
                        v, g = vg(jx)
                        acc += float(v)
                    return acc / reps, np.asarray(g)
                st, r = timed(run, *wt)
                emit(slug, reps, "jax", LIBV["jax"], wt, st, ok_of(r), "jax.jit(jax.value_and_grad(f)) (CPU); " + base + sn)
            except Exception as e:
                fail(slug, "jax", e)
    if wanted(slug, "torch"):
        if LIBV.get("torch") is None:
            skip(slug, "torch", "torch not installed in the venv (run competitors/setup.sh)")
        else:
            try:
                import torch
                from torch.func import grad_and_value
                gv = grad_and_value(lambda t: torch.sum(t * torch.exp(t)))
                tx = torch.from_numpy(x)

                def run():
                    acc = 0.0
                    g = None
                    for _ in range(reps):
                        g, v = gv(tx)
                        acc += float(v)
                    return acc / reps, g.numpy()
                st, r = timed(run, *wt)
                emit(slug, reps, "torch", LIBV["torch"], wt, st, ok_of(r), "torch.func.grad_and_value(f) (eager CPU); " + base + sn)
            except Exception as e:
                fail(slug, "torch", e)


def b_vmap():
    slug = "transforms-vmap"
    n = 1000000 if SMOKE else 10000000
    x = (u(np.arange(n)) - 0.5).astype(np.float32)
    xd = x.astype(np.float64)
    refv = xd * xd * 0.5 + xd * 0.25 - 1.0
    ref = float(refv.sum())
    sn = "; smoke size n=1e6 in place of 1e7" if SMOKE else ""
    base = "vmap of the per-element f(x) = x*x*0.5 + x*0.25 - 1 over 1e7 = 1e4 x 1e3 float32 (the Cajeta Vmap v1 shape: scalar bodies only); check: sum within 1e-5"
    wt = iters(2, 10)

    def ok_of(r):
        r = np.asarray(r)
        return (r.shape == (n,) and rel(float(r.astype(np.float64).sum()), ref) <= 1e-5
                and abs(float(r[0]) - refv[0]) <= 1e-6 and abs(float(r[-1]) - refv[-1]) <= 1e-6)

    if wanted(slug, "jax"):
        if LIBV.get("jax") is None:
            skip(slug, "jax", "jax not installed in the venv (run competitors/setup.sh)")
        else:
            try:
                import jax
                import jax.numpy as jnp
                fn = jax.jit(jax.vmap(lambda t: t * t * 0.5 + t * 0.25 - 1.0))
                jx = jnp.asarray(x)
                fn(jx).block_until_ready()
                st, r = timed(lambda: fn(jx).block_until_ready(), *wt)
                emit(slug, n, "jax", LIBV["jax"], wt, st, ok_of(r), "jax.jit(jax.vmap(f)) (CPU); " + base + sn)
            except Exception as e:
                fail(slug, "jax", e)
    if wanted(slug, "torch"):
        if LIBV.get("torch") is None:
            skip(slug, "torch", "torch not installed in the venv (run competitors/setup.sh)")
        else:
            try:
                import torch
                fn = torch.func.vmap(lambda t: t * t * 0.5 + t * 0.25 - 1.0)
                tx = torch.from_numpy(x)
                st, r = timed(lambda: fn(tx), *wt)
                emit(slug, n, "torch", LIBV["torch"], wt, st, ok_of(r.numpy()), "torch.func.vmap(f) (eager CPU); " + base + sn)
            except Exception as e:
                fail(slug, "torch", e)


def b_jit():
    slug = "transforms-jit"
    n = 100000 if SMOKE else 1000000
    x = u(np.arange(n)).astype(np.float32)
    xd = x.astype(np.float64)
    refv = ((xd * xd + xd) * 0.5 - xd / (xd + 1.0)) * (xd + 2.0) - xd * 0.25
    ref = float(refv.sum())
    sn = "; smoke size n=1e5 in place of 1e6" if SMOKE else ""
    base = "the 10-op f(x) = ((x*x + x)*0.5 - x/(x+1))*(x+2) - x*0.25 over n=1e6 float32, warm; check: sum within 1e-5"
    wt = iters(2, 10)

    def f(t):
        return ((t * t + t) * 0.5 - t / (t + 1.0)) * (t + 2.0) - t * 0.25

    def ok_of(r):
        r = np.asarray(r)
        return (r.shape == (n,) and rel(float(r.astype(np.float64).sum()), ref) <= 1e-5
                and abs(float(r[0]) - refv[0]) <= 1e-5 and abs(float(r[-1]) - refv[-1]) <= 1e-5)

    if wanted(slug, "jax"):
        if LIBV.get("jax") is None:
            skip(slug, "jax", "jax not installed in the venv (run competitors/setup.sh)")
        else:
            try:
                import jax
                import jax.numpy as jnp
                fn = jax.jit(f)
                jx = jnp.asarray(x)
                fn(jx).block_until_ready()
                st, r = timed(lambda: fn(jx).block_until_ready(), *wt)
                emit(slug, n, "jax", LIBV["jax"], wt, st, ok_of(r), "jax.jit(f) (XLA CPU; compiled before warmup); " + base + sn)
            except Exception as e:
                fail(slug, "jax", e)
    if wanted(slug, "torch"):
        if LIBV.get("torch") is None:
            skip(slug, "torch", "torch not installed in the venv (run competitors/setup.sh)")
        else:
            try:
                import torch
                fn = torch.compile(f)
                tx = torch.from_numpy(x)
                fn(tx)
                st, r = timed(lambda: fn(tx), *wt)
                emit(slug, n, "torch", LIBV["torch"], wt, st, ok_of(r.numpy()), "torch.compile(f) (inductor CPU; compiled before warmup); " + base + sn)
            except Exception as e:
                fail(slug, "torch", e)


BENCHES = [
    (("table-groupby", "table-join"), b_table_groupby_join),
    (("table-sort", "table-filter"), b_table_sort_filter),
    (("column-filter",), b_column_filter),
    (("column-arith",), b_column_arith),
    (("tape-forward-backward",), b_tape),
    (("fuse-elementwise-chain",), b_fuse),
    (("transforms-grad",), b_grad),
    (("transforms-vmap",), b_vmap),
    (("transforms-jit",), b_jit),
]


def main():
    LIBV.update(libs_present())
    for slugs, fn in BENCHES:
        if not any(wanted(s) for s in slugs):
            continue
        try:
            fn()
        except Exception as e:  # a failed row, then the next id
            for s in slugs:
                fail(s, "python", e)


if __name__ == "__main__":
    main()
