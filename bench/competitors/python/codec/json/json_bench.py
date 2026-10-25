#!/usr/bin/env python3
"""codec/json-* competitors in Python: stdlib json and orjson on the
nativejson corpus and the JSON_checker set (bench/codec/json/Json.md).
Neither library has a pull tokenizer or a typed binder, so json-tokenize and
json-bind-skip are recorded as skipped rows with that reason."""
import json
import os
import sys

import orjson

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _rows import env, measure, out, row, skip  # noqa: E402

FILES = ["twitter", "citm_catalog", "canada"]
NODES = {"twitter": 13914, "citm_catalog": 37778, "canada": 167179}
PASS = [f"pass{i:02d}" for i in range(1, 4)]
FAIL = [f"fail{i:02d}" for i in range(2, 34) if i != 18]


def nodes(v):
    """Every value in the tree, containers included, keys not counted."""
    n = 0
    stack = [v]
    while stack:
        x = stack.pop()
        n += 1
        if isinstance(x, dict):
            stack.extend(x.values())
        elif isinstance(x, list):
            stack.extend(x)
    return n


def libs():
    return [
        ("json", sys.version.split()[0], json.loads,
         lambda v: json.dumps(v, separators=(",", ":"), ensure_ascii=False).encode()),
        ("orjson", orjson.__version__, orjson.loads, orjson.dumps),
    ]


def main():
    d = os.environ.get("PROFILE_DATA_DIR")
    if not d:
        sys.stderr.write("python/codec/json: PROFILE_DATA_DIR unset\n")
        return 1
    warmup = int(env("PROFILE_WARMUP", "3"))
    trials = int(env("PROFILE_TRIALS", "10"))
    for name in FILES:
        with open(os.path.join(d, name + ".json"), "rb") as fh:
            raw = fh.read()
        n = len(raw)
        for lib, ver, loads, dumps in libs():
            out(skip("json-tokenize", name, n, n, lib, ver,
                     f"{lib} has no pull tokenizer (loads builds the whole tree)", name))
            out(skip("json-bind-skip", name, n, n, lib, ver,
                     f"{lib} has no typed binder (loads only builds dict/list trees)", name))

            st, v = measure(lambda: loads(raw), warmup, trials)
            c = nodes(v)
            out(row("json-dom", name, n, n, lib, ver, warmup, trials, st,
                    c == NODES[name], f"nodes={c}", name))

            tree = loads(raw)   # built untimed
            st, b = measure(lambda: dumps(tree), warmup, trials)
            c = nodes(loads(b))
            out(row("json-serialize", name, n, n, lib, ver, warmup, trials, st,
                    c == NODES[name], f"out={len(b)} bytes; reparsed nodes={c}", name))

            def rt():
                a = loads(raw)
                return a, loads(dumps(a))
            st, (a, b2) = measure(rt, warmup, trials)
            eq = a == b2
            out(row("json-roundtrip", name, n, n, lib, ver, warmup, trials, st,
                    eq, "trees equal" if eq else "trees differ", name))

    docs = []
    for nm in PASS + FAIL:
        with open(os.path.join(d, f"jsonchecker_{nm}.json"), "rb") as fh:
            docs.append((nm, fh.read()))
    for lib, ver, loads, _ in libs():
        def classify():
            acc = []
            for _nm, b in docs:
                try:
                    loads(b)
                    acc.append(True)
                except ValueError:   # json.JSONDecodeError and orjson.JSONDecodeError
                    acc.append(False)
            return acc
        st, acc = measure(classify, warmup, trials)
        npass = sum(1 for (nm, _), a in zip(docs, acc) if nm.startswith("pass") and a)
        nrej = sum(1 for (nm, _), a in zip(docs, acc) if nm.startswith("fail") and not a)
        bad = [nm for (nm, _), a in zip(docs, acc) if a != nm.startswith("pass")]
        note = f"pass {npass}/3 fail {nrej}/31"
        if bad:
            note += "; misclassified " + " ".join(bad)
        out(row("json-conformance", "jsonchecker", "", 34, lib, ver, warmup, trials, st,
                npass == 3 and nrej == 31, note, "", mbps=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
