#!/usr/bin/env python3
"""codec/csv-parse and codec/csv-write in Python: pandas read_csv/to_csv and
the stdlib csv module, on the file datasets/gen_csv.py generates
(bench/codec/csv/Csv.md). Rows are typed: id int64, name str, qty int32,
price float64, note str. Timed work runs on bytes already in memory."""
import csv
import io
import os
import sys

import numpy as np
import pandas as pd

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _rows import env, measure, out, row  # noqa: E402

DTYPES = {"id": np.int64, "name": str, "qty": np.int32, "price": np.float64, "note": str}


def stdlib_parse(raw):
    rd = csv.reader(io.StringIO(raw.decode("utf-8"), newline=""))
    next(rd)
    return [(int(r[0]), r[1], int(r[2]), float(r[3]), r[4]) for r in rd]


def stdlib_write(rows):
    buf = io.StringIO()
    w = csv.writer(buf, lineterminator="\n")
    w.writerow(("id", "name", "qty", "price", "note"))
    w.writerows(rows)
    return buf.getvalue().encode("utf-8")


def pandas_parse(raw):
    return pd.read_csv(io.BytesIO(raw), dtype=DTYPES, keep_default_na=False)


def pandas_write(df):
    return df.to_csv(index=False, lineterminator="\n").encode("utf-8")


def main():
    d = os.environ.get("PROFILE_DATA_DIR")
    if not d:
        sys.stderr.write("python/codec/csv: PROFILE_DATA_DIR unset\n")
        return 1
    smoke = env("CAJETA_BENCH_SMOKE", "") in ("1", "true", "yes")
    ds = "codec-csv-" + ("smoke" if smoke else "full")
    with open(os.path.join(d, ds + ".csv"), "rb") as fh:
        raw = fh.read()
    with open(os.path.join(d, ds + ".meta")) as fh:
        mrows, mqty, mbytes = (int(x) for x in fh.read().split())
    n = len(raw)
    warmup = int(env("PROFILE_WARMUP", "3"))
    trials = int(env("PROFILE_TRIALS", "10"))
    tag = "; smoke input 4 MB" if smoke else ""
    pyver = sys.version.split()[0]

    st, df = measure(lambda: pandas_parse(raw), warmup, trials)
    nr, q = len(df), int(df["qty"].astype(np.int64).sum())
    out(row("csv-parse", ds, n, n, "pandas", pd.__version__, warmup, trials, st,
            nr == mrows and q == mqty, f"read_csv with dtypes; rows={nr} qty_sum={q}{tag}", ""))
    df = pandas_parse(raw)
    st, b = measure(lambda: pandas_write(df), warmup, trials)
    out(row("csv-write", ds, n, n, "pandas", pd.__version__, warmup, trials, st,
            len(b) == mbytes, f"to_csv; out={len(b)} bytes{tag}", ""))

    st, rows = measure(lambda: stdlib_parse(raw), warmup, trials)
    nr, q = len(rows), sum(r[2] for r in rows)
    out(row("csv-parse", ds, n, n, "csv", pyver, warmup, trials, st,
            nr == mrows and q == mqty, f"csv.reader + int/float; rows={nr} qty_sum={q}{tag}", ""))
    rows = stdlib_parse(raw)
    st, b = measure(lambda: stdlib_write(rows), warmup, trials)
    out(row("csv-write", ds, n, n, "csv", pyver, warmup, trials, st,
            len(b) == mbytes, f"csv.writer to StringIO; out={len(b)} bytes{tag}", ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
