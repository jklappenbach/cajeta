#!/usr/bin/env python3
"""Do the filters, the durations table, and the binary agree on what exists?

test-battery-restructure 4.1.1. Every failure here is silent — nothing errors,
the build stays clean, and the sweep goes green over whatever it happened to run.

  DANGLING   a filter naming a test the binary lacks; gtest matches nothing and
             just runs less (spec 8.1).
  OVERLAP    a test in both the routine gate and stress; the runner drops it.
  UNTIMED    a gate test with no duration row, so it gets the flat TEST_TIMEOUT.
  REFERENCE  a test that drives another BY NAME in a string literal, where the
             named test is gone. The driver then fails on an unrelated assertion.
  STALE      the routine filter was derived against a different battery. Only the
             binary-is-LARGER direction is fatal: those tests are outside the
             gate, and `corpus - routine` turns them into a delete set.

    ./check_filters.py --binary build/test/cajeta_test --root .
    ./check_filters.py --selftest
"""

import argparse
import glob
import os
import re
import subprocess
import sys

BATTERY_LINE = re.compile(r"^#\s*Battery:\s*(\d+)\s+runnable")


def tests_from_binary(binary):
    out = subprocess.run([binary, "--gtest_list_tests"],
                         capture_output=True, text=True, check=True).stdout
    names, suite = set(), None
    for line in out.splitlines():
        if not line.strip() or line.startswith("Running"):
            continue
        if not line.startswith(" "):
            suite = line.strip()
            continue
        if suite:
            names.add(suite + line.strip().split("#")[0].strip())
    return names


def read_filter(path):
    names, battery = set(), None
    with open(path) as fh:
        for line in fh:
            m = BATTERY_LINE.match(line)
            if m:
                battery = int(m.group(1))
            line = line.strip()
            if line and not line.startswith("#"):
                names.add(line)
    return names, battery


REFERENCE_RX = re.compile(r'"([A-Za-z_]\w*)\.([A-Za-z0-9_*]+)"')


def references_in_sources(root, binary_tests):
    """Filter strings in test sources that name another test.

    `"Foo.bar"` is ambiguous — usually a `.cajeta` filename, not a test — so
    only `Suite.*`, or `Suite.name` whose suite exists, counts. GAP: a reference
    to an already-deleted suite is indistinguishable from a filename. The exact
    check lives in delete_tests.py, where the doomed set is known.
    """
    suites = {t.split(".", 1)[0] for t in binary_tests}
    refs = set()
    for dirpath, _, names in os.walk(os.path.join(root, "test")):
        for fn in names:
            if not fn.endswith((".cpp", ".cc", ".h")):
                continue
            with open(os.path.join(dirpath, fn), errors="ignore") as fh:
                for m in REFERENCE_RX.finditer(fh.read()):
                    suite, name = m.group(1), m.group(2)
                    if name == "*" or suite in suites:
                        refs.add(f"{suite}.{name}")
    return refs


def read_durations(path):
    names = set()
    if not os.path.exists(path):
        return names
    with open(path) as fh:
        for line in fh:
            parts = line.rstrip("\n").split("\t")
            if parts and parts[0]:
                names.add(parts[0])
    return names


def is_runnable(name):
    suite, _, test = name.partition(".")
    return not (suite.startswith("DISABLED_") or test.startswith("DISABLED_"))


def resolves(entry, binary_tests):
    """True if the entry matches a real test.

    Entries are gtest PATTERNS (`CompilerTests.*`), not plain names; comparing
    them literally reports every wildcard as dangling.
    """
    if "*" not in entry and "?" not in entry:
        return entry in binary_tests
    rx = re.compile("^" + re.escape(entry)
                    .replace(r"\*", ".*").replace(r"\?", ".") + "$")
    return any(rx.match(t) for t in binary_tests)


def check(binary_tests, filters, durations, routine_key="routine",
          references=None):
    """filters: {name: (set, battery_or_None)}. Returns (problems, notes)."""
    problems, notes = [], []
    for name, (names, battery) in sorted(filters.items()):
        dangling = {e for e in names if not resolves(e, binary_tests)}
        if dangling:
            problems.append((f"{name}: DANGLING", sorted(dangling)))

    # No routine filter: the gate is the corpus. An empty set here would make
    # the untimed/overlap checks vacuous.
    if routine_key in filters:
        routine = filters[routine_key][0]
    else:
        routine = {t for t in binary_tests if is_runnable(t)}
    stress = filters.get("stress", (set(), None))[0]
    # Only meaningful while a routine FILTER exists; with the corpus as the gate
    # every stress test is trivially "in" it.
    if routine_key in filters:
        overlap = routine & stress
        if overlap:
            problems.append(("routine ∩ stress", sorted(overlap)))

    untimed = {t for t in routine & binary_tests if is_runnable(t)} - durations
    if untimed:
        # Advisory: most untimed tests SKIP here (no CUDA/OptiX) and can never be
        # timed, and a permanently-red check is one nobody reads.
        notes.append(
            f"UNTIMED: {len(untimed)} gate test(s) have no duration row, so "
            "they get the flat TEST_TIMEOUT on their first run:\n    "
            + "\n    ".join(sorted(untimed)))

    if references is not None:
        broken = sorted(r for r in references if not resolves(r, binary_tests))
        if broken:
            problems.append(
                ("REFERENCE (a test names another test that does not exist)",
                 broken))

    runnable = {t for t in binary_tests if is_runnable(t)}
    if routine_key not in filters:
        notes.append("no routine_filter.txt — corpus IS the gate (spec 8.4)")
        return problems, notes
    battery = filters[routine_key][1]
    if battery is None:
        notes.append(f"{routine_key}: no 'Battery:' header — staleness "
                     "unverifiable; regenerate with build_routine.py")
    elif len(runnable) > battery:
        # Tests the derivation never saw: outside the gate by construction, and
        # a delete set to anyone computing `corpus - routine`.
        problems.append((
            f"STALE: {routine_key} derived against {battery} runnable tests, "
            f"binary now has {len(runnable)} — {len(runnable) - battery} the "
            f"derivation never saw", []))
    elif len(runnable) < battery:
        # Harmless: the filter is a superset, so nothing falls through.
        notes.append(
            f"{routine_key}: derived against {battery}, binary now has "
            f"{len(runnable)} (tests removed since) — re-derive to re-stamp")
    return problems, notes


def selftest():
    binary = {"A.keeps", "A.slow", "B.stress", "C.new"}
    filters = {
        "routine": ({"A.keeps", "A.slow", "B.stress", "A.gone"}, 4),
        "stress": ({"B.stress"}, None),
    }
    problems, notes = check(binary, filters, durations={"A.keeps"})
    kinds = [p[0] for p in problems]
    assert any("DANGLING" in k for k in kinds), kinds        # A.gone
    assert any("∩ stress" in k for k in kinds), kinds        # B.stress in both
    # UNTIMED is advisory (see the note in check()), so it lands in notes —
    # but it must still NAME the untimed tests and exclude the timed one.
    un = [n for n in notes if n.startswith("UNTIMED")]
    assert un, notes
    assert "A.slow" in un[0] and "A.keeps" not in un[0], un

    # Staleness is ASYMMETRIC. Binary bigger than the recorded battery means
    # tests exist the derivation never saw — the direction that becomes a
    # delete set. It must FAIL.
    grew = {"routine": ({"A.keeps"}, 2)}
    assert any("STALE" in p[0] for p in check(binary, grew, binary)[0])
    # Binary smaller means tests were deleted after deriving: the filter is a
    # superset, nothing falls through. It must NOT fail — only note.
    shrank = {"routine": ({"A.keeps"}, 99)}
    probs, notes = check(binary, shrank, binary)
    assert not any("STALE" in p[0] for p in probs), probs
    assert any("removed since" in n for n in notes), notes
    # ...and neither fires when the filter matches the battery.
    ok = {"routine": ({"A.keeps"}, 4), "stress": (set(), None)}
    assert not any("STALE" in p[0] for p in check(binary, ok, binary)[0])

    # gtest wildcards resolve by MATCH, not by literal membership.
    assert resolves("A.*", binary) and resolves("A.keeps", binary)
    assert not resolves("Nope.*", binary)
    pat = {"release": ({"A.*", "Nope.*"}, None)}
    dang = dict(check(binary, pat, binary)[0]).get("release: DANGLING", [])
    assert dang == ["Nope.*"], dang
    # A DISABLED test is not "untimed" — it never runs.
    dn = check({"A.keeps", "A.DISABLED_x"},
               {"routine": ({"A.keeps", "A.DISABLED_x"}, 1)}, {"A.keeps"})[1]
    assert not any("A.DISABLED_x" in n for n in dn if n.startswith("UNTIMED")), dn
    # A reference to a live test is fine; one to a deleted test must fire.
    r = check(binary, {"routine": ({"A.keeps"}, 4)}, binary,
              references={"A.keeps", "Gone.suite"})[0]
    assert any("REFERENCE" in p[0] for p in r), r
    assert dict(r)["REFERENCE (a test names another test that does not exist)"] \
        == ["Gone.suite"], r
    # Wildcards resolve the same way here.
    r = check(binary, {"routine": ({"A.keeps"}, 4)}, binary,
              references={"A.*"})[0]
    assert not any("REFERENCE" in p[0] for p in r), r

    print("selftest: ok")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary")
    ap.add_argument("--root", default=".")
    ap.add_argument("--show", type=int, default=10)
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if not a.binary:
        ap.error("--binary is required")

    binary_tests = tests_from_binary(a.binary)
    filters = {}
    for path in sorted(glob.glob(os.path.join(a.root, "test", "*_filter.txt"))):
        key = os.path.basename(path)[: -len("_filter.txt")]
        filters[key] = read_filter(path)

    durations = read_durations(os.path.join(a.root, ".test-durations.tsv"))
    durations |= read_durations(
        os.path.join(a.root, "test", "test-durations.seed.tsv"))

    print(f"binary            {len(binary_tests):>6}")
    for k, (names, battery) in sorted(filters.items()):
        b = f"  (derived against {battery})" if battery else ""
        print(f"{k + '_filter':<18}{len(names):>6}{b}")
    print(f"durations rows    {len(durations):>6}")

    references = references_in_sources(a.root, binary_tests)
    print(f"test-to-test refs {len(references):>6}")
    print()
    problems, notes = check(binary_tests, filters, durations,
                            references=references)
    for note in notes:
        print(f"NOTE: {note}")
    if not problems:
        print("OK — filters, durations and binary agree")
        return 0
    for label, items in problems:
        print(f"\n{label}: {len(items)}")
        for i in items[: a.show]:
            print(f"    {i}")
        if len(items) > a.show:
            print(f"    ... and {len(items) - a.show} more")
    return 1


if __name__ == "__main__":
    sys.exit(main())
