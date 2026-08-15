#!/usr/bin/env python3
"""Do the corpus, the routine gate, and the coverage index describe the same tests?

test-battery-restructure 1.1.1. Every claim downstream of the coverage measure —
"81% hold zero unique lines", "879 tests retain 99.5%", and every fold/promote/
delete disposition in plan unit 3 — is a set operation over `.coverage/index`.
If that index describes a corpus the binary no longer has, those claims are
about tests that may not exist and silently omit tests that do. Nothing errors;
the numbers just quietly stop being about this codebase.

Three sources, one question:

  binary   `--gtest_list_tests`      the corpus of record
  routine  test/routine_filter.txt   what the everyday sweep runs
  index    .coverage/index/*.json    what the measure knows about

Disagreement is reported as three sets, because they mean different things:

  UNMEASURED  in the binary, not in the index. Every disposition about these is
              uninformed — this is what makes an index STALE.
  PHANTOM     in the index, not in the binary. Deleted or renamed since the
              measure; their lines are still counted toward coverage totals
              that no longer hold.
  DANGLING    named by the routine filter, absent from the binary. A gtest
              filter naming a missing test matches nothing and simply runs
              less, with no error (spec §8.1).

Exit 0 only when all three are empty.

    ./check_index_freshness.py --binary build/test/cajeta_test \\
        --index /path/to/.coverage/index --routine test/routine_filter.txt
    ./check_index_freshness.py --selftest
"""

import argparse
import json
import os
import subprocess
import sys
import tempfile


def tests_from_binary(binary):
    """Fully-qualified Suite.test names from --gtest_list_tests."""
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
            # gtest appends "# GetParam() = .." on parameterised rows.
            names.add(suite + line.strip().split("#")[0].strip())
    return names


def tests_from_routine(path):
    names = set()
    with open(path) as fh:
        for line in fh:
            line = line.strip()
            if line and not line.startswith("#"):
                names.add(line)
    return names


def tests_from_index(index_dir):
    """Unit names from the per-test JSON files.

    Read from the FILENAME, not by parsing 5,868 JSON bodies — the body's
    `files` map is large (the whole point of the index) and this check does not
    need it. A body is opened only when the filename is not the `.json` shape.
    """
    names = set()
    for entry in os.listdir(index_dir):
        if entry.endswith(".json"):
            names.add(entry[: -len(".json")])
        else:
            with open(os.path.join(index_dir, entry)) as fh:
                names.add(json.load(fh)["unit"])
    return names


def is_disabled(name):
    """gtest skips DISABLED_ on suite or test, so these never produce coverage."""
    suite, _, test = name.partition(".")
    return suite.startswith("DISABLED_") or test.startswith("DISABLED_")


def compare(binary_tests, routine_tests, index_tests):
    # DISABLED tests are enumerated but never executed, so they can never be
    # measured. Counting them as UNMEASURED makes a clean index look permanently
    # stale — the check would cry wolf forever and get ignored, which is worse
    # than not having it. They get their own line instead, because "in the
    # corpus, never runs, covers nothing" is its own defect: for the corpus ==
    # gate endgame each one still needs a disposition (re-enable or delete).
    disabled = {t for t in binary_tests if is_disabled(t)}
    live = binary_tests - disabled
    return {
        "unmeasured": live - index_tests,
        "phantom": index_tests - binary_tests,
        "dangling": routine_tests - binary_tests,
        "disabled": disabled,
    }


def report(counts, sets, show):
    print(f"corpus (binary)        {counts['binary']:>6}")
    print(f"routine gate           {counts['routine']:>6}")
    print(f"coverage index         {counts['index']:>6}")
    print()
    ok = True
    for key, label in (("unmeasured", "UNMEASURED (in binary, not indexed)"),
                       ("phantom", "PHANTOM    (indexed, not in binary)"),
                       ("dangling", "DANGLING   (in routine, not in binary)")):
        items = sorted(sets[key])
        print(f"{label}: {len(items)}")
        if items:
            ok = False
            for name in items[:show]:
                print(f"    {name}")
            if len(items) > show:
                print(f"    ... and {len(items) - show} more")
    # Reported, never fatal: a DISABLED test is a disposition question, not a
    # stale index.
    dis = sorted(sets["disabled"])
    print(f"DISABLED   (enumerated, never runs, unmeasurable): {len(dis)}")
    for name in dis[:show]:
        print(f"    {name}")
    if len(dis) > show:
        print(f"    ... and {len(dis) - show} more")
    return ok


def selftest():
    """The detector must fire. A freshness check that cannot go red is decoration."""
    with tempfile.TemporaryDirectory() as tmp:
        index = os.path.join(tmp, "index")
        os.makedirs(index)
        for unit in ("A.keeps", "A.deletedSinceMeasure"):
            with open(os.path.join(index, unit + ".json"), "w") as fh:
                json.dump({"unit": unit, "files": {}}, fh)

        binary_tests = {"A.keeps", "A.addedSinceMeasure"}
        routine_tests = {"A.keeps", "A.renamedAway"}
        sets = compare(binary_tests, routine_tests, tests_from_index(index))

        assert sets["unmeasured"] == {"A.addedSinceMeasure"}, sets["unmeasured"]
        assert sets["phantom"] == {"A.deletedSinceMeasure"}, sets["phantom"]
        assert sets["dangling"] == {"A.renamedAway"}, sets["dangling"]

        # A DISABLED test must not read as a stale index.
        d = compare({"A.keeps", "A.DISABLED_off"}, {"A.keeps"}, {"A.keeps"})
        assert d["unmeasured"] == set(), d["unmeasured"]
        assert d["disabled"] == {"A.DISABLED_off"}, d["disabled"]

        # And it must go GREEN when the three agree, or it is just an alarm.
        agreed = compare({"A.keeps"}, {"A.keeps"}, {"A.keeps"})
        assert not any(agreed[k] for k in ("unmeasured", "phantom", "dangling")), agreed
    print("selftest: ok")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary")
    ap.add_argument("--index")
    ap.add_argument("--routine")
    ap.add_argument("--show", type=int, default=10)
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()

    if args.selftest:
        return selftest()
    if not (args.binary and args.index and args.routine):
        ap.error("--binary, --index and --routine are required")

    binary_tests = tests_from_binary(args.binary)
    routine_tests = tests_from_routine(args.routine)
    index_tests = tests_from_index(args.index)
    sets = compare(binary_tests, routine_tests, index_tests)
    counts = {"binary": len(binary_tests), "routine": len(routine_tests),
              "index": len(index_tests)}
    return 0 if report(counts, sets, args.show) else 1


if __name__ == "__main__":
    sys.exit(main())
