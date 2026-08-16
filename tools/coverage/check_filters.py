#!/usr/bin/env python3
"""Do the filters, the durations table, and the binary agree on what exists?

test-battery-restructure 4.1.1. Every failure this guards is SILENT — nothing
errors, the build stays clean, and the sweep goes green over whatever it
happened to run.

  DANGLING     a filter naming a test the binary does not have. gtest matches
               nothing and simply runs less (spec §8.1). Found live on
               2026-08-15: `KernelCellTests.primitiveBindingAcrossCellsRefuses-
               Loudly` was renamed by 219c7adb and had not run in the everyday
               sweep since.

  OVERLAP      a test in both the routine gate and the stress battery. The
               runner excludes stress from the sweep, so the routine entry is a
               lie about what runs.

  UNTIMED      a gate test with no row in the durations table. `run_one_test`
               falls back to the flat TEST_TIMEOUT, which killed 4 tests on the
               2026-08-15 gate — including the largest single coverage
               contributor in the battery. Every newly-promoted test arrives in
               this state, so this is a routine hazard, not an edge case.

  REFERENCE    a test that drives ANOTHER test by name through a
               `--gtest_filter` string, where the named test no longer exists.
               gtest warns and runs nothing, so the driving test fails with an
               unrelated-looking assertion. Found on 2026-08-15 when the
               deletion removed `ZoneOffsetTests`, which
               `ForkPerTestModeTests` drives to verify the fork-per-test
               harness: the failure read `Expected: (serialOk) > (0)`, naming
               neither the deleted suite nor the deletion. This dependency
               lives in a STRING LITERAL — no filter, index or coverage measure
               can see it.

  STALE        the routine filter was derived from a different battery than the
               one that exists now. THIS IS THE EXPENSIVE ONE. `routine_filter`
               is DERIVED from `.coverage/index`; refresh the index without
               re-running build_routine.py and the filter silently describes an
               older corpus. On 2026-08-15 that turned `corpus - routine -
               stress` into a delete set that removed 4,607 tests including
               that session's own regression guards — every one of which looked
               fine, built clean, and passed the gate.

`build_routine.py` records the battery size it derived against in the filter
header; this compares that against the binary. A filter with no such header
cannot be checked and is reported, not assumed good.

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
    """gtest filter strings that name another test, found in test sources.

    A string literal `"Foo.bar"` is ambiguous: in this repo it is far more
    often a `.cajeta` SOURCE FILENAME than a test reference. Over-collecting
    and letting `resolves()` decide produced 487 false positives (`A.cajeta`,
    `App.cajeta`, ...) that buried the single real one — the same flood the
    wildcard bug caused in `release_filter`, so the rule is narrowed here:

      *  `Suite.*` is always a reference. Nothing else uses that shape, and it
         is the idiom a driving test actually writes.
      *  `Suite.name` counts only when `Suite` exists in the binary, which
         rules out filenames (there is no suite `App`) at the cost below.

    KNOWN GAP: a reference to `DeletedSuite.specificTest` — fully qualified,
    suite already gone — is NOT caught, because nothing distinguishes it from a
    filename once its suite no longer exists. The precise version of this check
    runs at DELETION time, where the delete set names exactly what is about to
    disappear and no guessing is required.
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
    """Does this filter entry name at least one real test?

    Entries are gtest filter patterns, not plain names: `release_filter.txt` is
    written as `CompilerTests.*`. Comparing those literally reports every one as
    dangling — which is what the first version of this tool did, turning a
    49-entry file into 49 false alarms and burying the 4 real ones next to it.
    A pattern is dangling only when it matches NOTHING.
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

    routine = filters.get(routine_key, (set(), None))[0]
    stress = filters.get("stress", (set(), None))[0]
    overlap = routine & stress
    if overlap:
        problems.append(("routine ∩ stress", sorted(overlap)))

    untimed = {t for t in routine & binary_tests if is_runnable(t)} - durations
    if untimed:
        problems.append(("UNTIMED in gate (flat TEST_TIMEOUT applies)",
                         sorted(untimed)))

    if references is not None:
        broken = sorted(r for r in references if not resolves(r, binary_tests))
        if broken:
            problems.append(
                ("REFERENCE (a test names another test that does not exist)",
                 broken))

    battery = filters.get(routine_key, (set(), None))[1]
    runnable = {t for t in binary_tests if is_runnable(t)}
    if battery is None:
        notes.append(f"{routine_key}: no 'Battery:' header — staleness "
                     "unverifiable; regenerate with build_routine.py")
    elif len(runnable) > battery:
        # THE DANGEROUS DIRECTION. The binary has tests the derivation never
        # saw, so they are absent from the gate by construction — and if anyone
        # computes `corpus - routine` they are a delete set. This is exactly
        # what removed 4,607 tests on 2026-08-15.
        problems.append((
            f"STALE: {routine_key} derived against {battery} runnable tests, "
            f"binary now has {len(runnable)} — {len(runnable) - battery} the "
            f"derivation never saw", []))
    elif len(runnable) < battery:
        # The harmless direction: tests were deleted after the derivation, so
        # the filter is a superset. Worth re-stamping, but nothing can fall
        # through the gap. Reported as a note so it does not mask the above.
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
    assert any("UNTIMED" in k for k in kinds), kinds         # A.slow, B.stress
    untimed = dict(problems)["UNTIMED in gate (flat TEST_TIMEOUT applies)"]
    assert "A.slow" in untimed and "A.keeps" not in untimed, untimed

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
    d = check({"A.keeps", "A.DISABLED_x"},
              {"routine": ({"A.keeps", "A.DISABLED_x"}, 1)}, {"A.keeps"})[0]
    assert not any("UNTIMED" in p[0] for p in d), d
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
