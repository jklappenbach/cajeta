#!/usr/bin/env python3
"""Derive behaviour pins from history: which tests were written to guard a fix?

test-battery-restructure unit 3. A coverage-derived gate is structurally blind
to assertions. `KernelSessionTests.aSessionsStructNamesDoNotLeakIntoTheNext`
covers zero unique lines — the leak it guards is a WRONG VALUE on lines other
tests already execute — so a greedy line-cover drops it and the corpus loses
the only thing standing between that bug and its return. Spec §5.1 caveat 1.

`regression_filter.txt` is the spec's answer ("line-redundant by design"), but
it is hand-curated and was never extended as new tests landed. This derives the
same signal mechanically instead:

    a test introduced by a commit that ALSO changed src/ was written to pin
    that change.

One reverse walk of `git log -p` over the test sources attributes each test to
the commit that first added it; a second pass asks whether that commit touched
src/. No per-test judgement, no LLM in the loop, reproducible by anyone.

WHY --max-src-files EXISTS, and why it is not optional in practice: a bulk
import or a sweeping refactor touches src/ and adds hundreds of tests at once,
and those tests are not pinning anything in particular. Without a focus bound
such a commit pins its entire contribution and the filter degenerates toward
"every test". A fix is small: it touches a few source files and adds the test
that proves it. The bound is what distinguishes the two, so report the
distribution before trusting any single value.

    ./derive_pins.py --root . --max-src-files 8 --out /tmp/pins.txt
    ./derive_pins.py --root . --histogram
    ./derive_pins.py --selftest
"""

import argparse
import os
import re
import subprocess
import sys

TEST_ADD = re.compile(
    r"^\+(TEST|TEST_F|TEST_P)\s*\(\s*([A-Za-z_]\w*)\s*,\s*([A-Za-z_]\w*)\s*\)")
COMMIT = re.compile(r"^\x01([0-9a-f]{40})$")


def git(root, *args):
    return subprocess.run(["git", "-C", root, *args],
                          capture_output=True, text=True, check=True).stdout


def first_adding_commit(root):
    """test name -> sha of the commit that first added its TEST macro.

    --reverse walks oldest-first, so the first sighting of a macro is its
    introduction. A test deleted and re-added keeps the ORIGINAL commit, which
    is the conservative reading: if it ever pinned a fix, it still does.
    """
    out = git(root, "log", "--reverse", "--format=\x01%H", "-p",
              "--no-renames", "--", "test/*.cpp", "test/*.cc")
    added, sha = {}, None
    for line in out.splitlines():
        m = COMMIT.match(line)
        if m:
            sha = m.group(1)
            continue
        m = TEST_ADD.match(line)
        if m and sha:
            added.setdefault(f"{m.group(2)}.{m.group(3)}", sha)
    return added


def src_touch_counts(root):
    """sha -> number of src/ files it changed (absent = touched none)."""
    out = git(root, "log", "--format=\x01%H", "--name-only", "--no-renames",
              "--", "src/")
    counts, sha = {}, None
    for line in out.splitlines():
        m = COMMIT.match(line)
        if m:
            sha = m.group(1)
            continue
        if line.strip() and sha:
            counts[sha] = counts.get(sha, 0) + 1
    return counts


def derive(added, counts, max_src_files):
    pins, skipped_broad = set(), 0
    for test, sha in added.items():
        n = counts.get(sha, 0)
        if n == 0:
            continue                       # test-only commit: pins nothing
        if max_src_files and n > max_src_files:
            skipped_broad += 1             # bulk import / sweeping refactor
            continue
        pins.add(test)
    return pins, skipped_broad


def selftest():
    added = {"A.fix": "s1", "B.bulk": "s2", "C.testonly": "s3"}
    counts = {"s1": 2, "s2": 400}          # s3 touched no src
    pins, broad = derive(added, counts, 8)
    assert pins == {"A.fix"}, pins
    assert broad == 1, broad
    # With no bound the bulk commit's tests come along — the failure mode the
    # bound exists to prevent.
    pins, broad = derive(added, counts, 0)
    assert pins == {"A.fix", "B.bulk"}, pins
    assert broad == 0, broad
    # Parsing: an added macro counts, a removed or context one never does.
    assert TEST_ADD.match("+TEST_F(Suite, name) {")
    assert not TEST_ADD.match("-TEST_F(Suite, name) {")
    assert not TEST_ADD.match(" TEST_F(Suite, name) {")
    print("selftest: ok")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=".")
    ap.add_argument("--max-src-files", type=int, default=8,
                    help="0 disables the focus bound (see the docstring)")
    ap.add_argument("--battery", help="restrict to tests in this list")
    ap.add_argument("--histogram", action="store_true")
    ap.add_argument("--out")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        return selftest()

    added = first_adding_commit(a.root)
    counts = src_touch_counts(a.root)
    if a.battery:
        live = {l.strip() for l in open(a.battery) if l.strip()}
        added = {t: s for t, s in added.items() if t in live}
    print(f"tests attributed to an adding commit: {len(added)}", file=sys.stderr)

    if a.histogram:
        print("\nmax-src-files  pins   (cumulative)", file=sys.stderr)
        for bound in (1, 2, 3, 5, 8, 12, 20, 50, 0):
            pins, broad = derive(added, counts, bound)
            label = "none" if bound == 0 else str(bound)
            print(f"  {label:>10}  {len(pins):>5}   "
                  f"({len(pins) / max(1, len(added)):.0%} of attributed)",
                  file=sys.stderr)
        return 0

    pins, broad = derive(added, counts, a.max_src_files)
    print(f"pins: {len(pins)}  (excluded {broad} from commits touching "
          f">{a.max_src_files} src files)", file=sys.stderr)
    if a.out:
        with open(a.out, "w") as fh:
            fh.write(
                "# Behaviour pins derived from history by "
                "tools/coverage/derive_pins.py.\n"
                "#\n"
                "# A test introduced by a commit that also changed src/ was\n"
                "# written to pin that change. These are line-redundant by\n"
                "# design (spec §5.1 caveat 1) — a coverage-derived gate drops\n"
                "# them, and the assertion goes with them.\n"
                f"# Focus bound: commits touching <= {a.max_src_files} src "
                "files.\n")
            for t in sorted(pins):
                fh.write(t + "\n")
        print(f"wrote {a.out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
