#!/usr/bin/env python3
"""Graft a per-test coverage index from another clone into this one.

test-battery-restructure 1.2.1. The 2026-08-10 measure lives in a sibling
clone; only 278 of 6,056 tests are unmeasured and 90 indexed units no longer
exist, so re-deriving 5,778 unchanged line sets (instrumented -O0 build plus
2h49m at 32 servers) buys nothing.

Two transformations, both of which matter:

  PATHS      The source index records absolute paths into ITS clone
             (/home/julian/code/cpp/cajeta-six/src/...). Copied verbatim they
             name files that do not exist here, and every set operation over
             them silently returns empty. Rewritten to repo-relative, they
             work in any clone — including the next one.
  PHANTOMS   Units the current binary no longer has are dropped. Keeping them
             inflates coverage totals with lines that no test reaches.

What this does NOT do is invent coverage for the unmeasured tests. After a
graft the freshness check still reports them; measuring them is 1.2.3.

    ./graft_index.py --from <src>/.coverage/index --into .coverage/index \\
        --src-root <src> --binary build/test/cajeta_test
    ./graft_index.py --selftest
"""

import argparse
import json
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check_index_freshness import tests_from_binary  # noqa: E402


def rewrite(unit_json, src_root):
    """Absolute paths under src_root become repo-relative; others are kept.

    A path outside src_root is left alone deliberately — it is a real signal
    (a generated file, a system header) and rewriting it to something
    plausible-looking would hide that.
    """
    src_root = src_root.rstrip("/") + "/"
    files, outside = {}, 0
    for path, lines in unit_json.get("files", {}).items():
        if path.startswith(src_root):
            files[path[len(src_root):]] = lines
        else:
            files[path] = lines
            outside += 1
    unit_json["files"] = files
    return unit_json, outside


def absolutise(unit_json, root):
    """Repo-relative paths become absolute under `root`.

    The in-clone form is ABSOLUTE, because that is what `analyze.py` expects:
    it realpath()s index entries to compare them against a filesystem walk, and
    splits on `/src/cajeta/` for display. Relative entries survive the first and
    silently mis-render in the second. Portability lives in this tool, not in
    the index — the index is a gitignored machine-local artifact.
    """
    root = root.rstrip("/") + "/"
    unit_json["files"] = {
        (path if path.startswith("/") else root + path): lines
        for path, lines in unit_json.get("files", {}).items()
    }
    return unit_json


def graft(src, dst, src_root, keep):
    os.makedirs(dst, exist_ok=True)
    written = dropped = outside_total = 0
    for entry in sorted(os.listdir(src)):
        if not entry.endswith(".json"):
            continue
        unit = entry[: -len(".json")]
        if keep is not None and unit not in keep:
            dropped += 1
            continue
        with open(os.path.join(src, entry)) as fh:
            data = json.load(fh)
        if src_root:
            data, outside = rewrite(data, src_root)
            outside_total += outside
        with open(os.path.join(dst, entry), "w") as fh:
            json.dump(data, fh)
        written += 1
    return written, dropped, outside_total


def selftest():
    with tempfile.TemporaryDirectory() as tmp:
        src, dst = os.path.join(tmp, "src"), os.path.join(tmp, "dst")
        os.makedirs(src)
        root = "/elsewhere/cajeta-six"
        with open(os.path.join(src, "A.lives.json"), "w") as fh:
            json.dump({"unit": "A.lives", "files": {
                f"{root}/src/cajeta/X.cpp": [1, 2],
                "/usr/include/foreign.h": [9],
            }}, fh)
        with open(os.path.join(src, "A.phantom.json"), "w") as fh:
            json.dump({"unit": "A.phantom", "files": {}}, fh)

        written, dropped, outside = graft(src, dst, root, {"A.lives"})
        assert (written, dropped, outside) == (1, 1, 1), (written, dropped, outside)

        with open(os.path.join(dst, "A.lives.json")) as fh:
            got = json.load(fh)["files"]
        # Rewritten to repo-relative...
        assert "src/cajeta/X.cpp" in got, got
        # ...and the foreign path preserved rather than mangled.
        assert "/usr/include/foreign.h" in got, got
        assert not os.path.exists(os.path.join(dst, "A.phantom.json"))

        # And back: relative -> absolute, leaving already-absolute alone.
        got = absolutise({"files": {"src/a.cpp": [1], "/opt/b.h": [2]}},
                         "/repo")["files"]
        assert got == {"/repo/src/a.cpp": [1], "/opt/b.h": [2]}, got
    print("selftest: ok")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--from", dest="src")
    ap.add_argument("--into", dest="dst")
    ap.add_argument("--src-root", help="clone root the source index refers to")
    ap.add_argument("--binary", help="drop units this binary no longer has")
    ap.add_argument("--to-absolute", metavar="ROOT",
                    help="rewrite repo-relative paths to absolute under ROOT "
                         "(the in-clone form analyze.py expects)")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()

    if args.selftest:
        return selftest()
    if args.to_absolute:
        n = 0
        for entry in sorted(os.listdir(args.src)):
            if not entry.endswith(".json"):
                continue
            path = os.path.join(args.src, entry)
            with open(path) as fh:
                data = json.load(fh)
            with open(path, "w") as fh:
                json.dump(absolutise(data, args.to_absolute), fh)
            n += 1
        print(f"absolutised {n}")
        return 0
    if not (args.src and args.dst):
        ap.error("--from and --into are required")
    # --src-root is optional: omitting it prunes/copies without rewriting,
    # which is what a same-clone prune wants. Requiring it forced callers to
    # pass a bogus root, and a bogus root silently reclassifies every path as
    # "outside" — a misleading count on an otherwise correct run.

    keep = tests_from_binary(args.binary) if args.binary else None
    written, dropped, outside = graft(args.src, args.dst, args.src_root, keep)
    print(f"grafted  {written}")
    print(f"phantoms dropped {dropped}")
    print(f"paths left absolute (outside src-root) {outside}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
