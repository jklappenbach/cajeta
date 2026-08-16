#!/usr/bin/env python3
"""Remove named gtest cases from the sources.

Brace-matches each body with a scanner that understands `//`, `/* */`, char and
string literals, and `R"tag(...)tag"` — a regex truncates a file at the first
brace inside a string and takes surviving tests with it. A file whose cases are
all removed is deleted.

Does NOT tidy up: helpers left unreferenced stay put. A tool guessing which
helper "belonged" to a deleted test removes things surviving tests need; the
build arbitrates.

    ./delete_tests.py --list .coverage/delete_set.txt --root test [--apply]
    ./delete_tests.py --selftest
"""

import argparse
import os
import re
import sys

# Never descend into these: nested worktrees and vendored trees hold second
# copies of the test tree, and deleting out of one is invisible in git status.
SKIP_DIRS = {'.git', '.claude', '.cache', 'node_modules', '_deps',
             'build', 'build-cov', 'build-tsan', 'cmake-build-debug'}

# Anchored to column 0: every test macro in this repo starts there, and the
# anchor rules out `// TEST(...)` and `"TEST(...)"` decoys.
MACRO = re.compile(
    r"^(TEST|TEST_F|TEST_P)\s*\(\s*([A-Za-z_]\w*)\s*,\s*([A-Za-z_]\w*)\s*\)",
    re.MULTILINE)


def end_of_body(src, start):
    """Index just past the `}` closing the body that opens at/after `start`.

    Returns None if unbalanced. The scanner tracks C++ lexical context so a
    brace inside a string, char literal, comment or raw string never counts.
    """
    i = src.find("{", start)
    if i < 0:
        return None
    depth, n = 0, len(src)
    while i < n:
        c = src[i]
        if c == "/" and i + 1 < n and src[i + 1] == "/":
            i = src.find("\n", i)
            if i < 0:
                return None
            continue
        if c == "/" and i + 1 < n and src[i + 1] == "*":
            j = src.find("*/", i + 2)
            if j < 0:
                return None
            i = j + 2
            continue
        if c == "R" and i + 1 < n and src[i + 1] == '"':
            m = re.match(r'R"([^(]*)\(', src[i:])
            if m:
                close = ')' + m.group(1) + '"'
                j = src.find(close, i + m.end())
                if j < 0:
                    return None
                i = j + len(close)
                continue
        if c in "\"'":
            quote, i = c, i + 1
            while i < n:
                if src[i] == "\\":
                    i += 2
                    continue
                if src[i] == quote:
                    i += 1
                    break
                i += 1
            continue
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    return None


def strip_tests(src, doomed):
    """Remove the doomed (suite, name) cases. Returns (text, removed, kept)."""
    out, removed, kept, pos = [], [], [], 0
    for m in MACRO.finditer(src):
        if m.start() < pos:
            continue
        suite, name = m.group(2), m.group(3)
        end = end_of_body(src, m.end())
        if end is None:
            continue
        if (suite, name) in doomed:
            out.append(src[pos:m.start()])
            removed.append(f"{suite}.{name}")
            # Swallow one trailing blank line so deletions do not leave gaps.
            while end < len(src) and src[end] == "\n":
                end += 1
                if end < len(src) and src[end] == "\n":
                    break
            pos = end
        else:
            kept.append(f"{suite}.{name}")
    out.append(src[pos:])
    return "".join(out), removed, kept


def selftest():
    src = '''
// TEST(Fake, inAComment) { }
const char* s = "TEST(Fake, inAString) {";
TEST(A, doomed) {
    if (x) { y("}"); }   // a brace in a string, and one in a comment: }
    const char* r = R"raw( } TEST(A, inRaw) { )raw";
}
TEST(A, survives) { EXPECT_EQ(1, 1); }
TEST_F(B, alsoDoomed) { { { } } }
'''
    out, removed, kept = strip_tests(src, {("A", "doomed"), ("B", "alsoDoomed")})
    assert removed == ["A.doomed", "B.alsoDoomed"], removed
    assert kept == ["A.survives"], kept
    assert "A.survives" not in out and "TEST(A, survives)" in out
    assert "doomed" not in out.replace("inAComment", ""), out
    # The string and comment decoys must be untouched.
    assert 'TEST(Fake, inAString)' in out
    assert 'inAComment' in out
    # Unbalanced input must be refused, not half-cut.
    assert end_of_body("TEST(A,b) { {", 0) is None
    # The reference guard must fire when a doomed suite is named elsewhere,
    # and stay quiet when a survivor keeps the reference valid.
    import tempfile
    with tempfile.TemporaryDirectory() as t:
        open(os.path.join(t, "driver.cpp"), "w").write(
            'run("ZoneOffsetTests.*"); auto f = "App.cajeta";\n')
        hits = referenced_by_other_tests(t, {("ZoneOffsetTests", "hoursMinutes")})
        assert "ZoneOffsetTests.*" in hits, hits
        # A .cajeta filename must never be mistaken for a test reference.
        assert not any(h.startswith("App.") for h in hits), hits
        # If one test of the suite survives, the wildcard still resolves.
        hits = referenced_by_other_tests(
            t, {("ZoneOffsetTests", "hoursMinutes"), ("Other", "x")})
        assert "ZoneOffsetTests.*" in hits, hits
    # SKIP_DIRS must keep the walk out of nested worktrees and vendored trees.
    with tempfile.TemporaryDirectory() as t:
        os.makedirs(os.path.join(t, ".claude", "worktrees", "baseline"))
        os.makedirs(os.path.join(t, "test"))
        body = 'TEST(A, doomed) { }\n'
        open(os.path.join(t, "test", "real.cpp"), "w").write(body)
        open(os.path.join(t, ".claude", "worktrees", "baseline", "copy.cpp"),
             "w").write(body)
        hits = referenced_by_other_tests(t, {("A", "doomed")})
        assert not any("baseline" in p for ps in hits.values() for p in ps), hits
    print("selftest: ok")
    return 0


REFERENCE_RX = re.compile(r'"([A-Za-z_]\w*)\.([A-Za-z0-9_*]+)"')


def referenced_by_other_tests(root, doomed):
    """Doomed tests that another test names in a string literal.

    Some tests drive others by gtest filter; deleting the subject makes the
    driver fail on an unrelated assertion. Exact here, because `doomed` names
    what is about to disappear — no heuristics needed.
    """
    doomed_suites = {s for s, _ in doomed}
    hits = {}
    for dirpath, dirnames, names in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        for fn in names:
            if not fn.endswith((".cpp", ".cc", ".h")):
                continue
            path = os.path.join(dirpath, fn)
            with open(path, errors="ignore") as fh:
                text = fh.read()
            for m in REFERENCE_RX.finditer(text):
                suite, name = m.group(1), m.group(2)
                if name == "*":
                    # Whole suite: a problem only if every test in it dies.
                    if suite in doomed_suites and not any(
                            s == suite and (s, n) not in doomed
                            for s, n in doomed):
                        hits.setdefault(f"{suite}.*", set()).add(path)
                elif (suite, name) in doomed:
                    hits.setdefault(f"{suite}.{name}", set()).add(path)
    return hits


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--list")
    ap.add_argument("--root", default="test")
    ap.add_argument("--apply", action="store_true")
    ap.add_argument("--allow-repo-root", action="store_true",
                    help="permit --root to be the repo root (see SKIP_DIRS)")
    ap.add_argument("--force", action="store_true",
                    help="delete even when another test references the set")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()
    if args.selftest:
        return selftest()

    doomed = set()
    for line in open(args.list):
        line = line.strip()
        if line and "." in line:
            s, _, n = line.partition(".")
            doomed.add((s, n))

    if not os.path.isdir(args.root):
        print(f"error: --root {args.root!r} is not a directory", file=sys.stderr)
        return 2
    if os.path.realpath(args.root) == os.path.realpath(os.getcwd()) \
            and not args.allow_repo_root:
        print("error: --root is the repository root. Deleting test cases "
              "repo-wide reaches nested worktrees and vendored copies; pass "
              "an explicit subtree (e.g. --root test), or --allow-repo-root "
              "if you really mean it.", file=sys.stderr)
        return 2

    refs = referenced_by_other_tests(args.root, doomed)
    if refs:
        print("REFUSING: these doomed tests are named by other tests; "
              "deleting them breaks those tests silently.", file=sys.stderr)
        for ref, paths in sorted(refs.items()):
            for p in sorted(paths):
                print(f"    {ref}  <- {p}", file=sys.stderr)
        if not args.force:
            print("Pin them, or re-run with --force.", file=sys.stderr)
            return 2

    files_changed = files_deleted = total_removed = 0
    for dirpath, dirnames, names in os.walk(args.root):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        for fn in names:
            if not fn.endswith((".cpp", ".cc")):
                continue
            path = os.path.join(dirpath, fn)
            src = open(path).read()
            if not MACRO.search(src):
                continue
            out, removed, kept = strip_tests(src, doomed)
            if not removed:
                continue
            total_removed += len(removed)
            if kept:
                files_changed += 1
                if args.apply:
                    open(path, "w").write(out)
            else:
                files_deleted += 1
                if args.apply:
                    os.remove(path)
    verb = "removed" if args.apply else "would remove"
    print(f"{verb} {total_removed} tests; "
          f"{files_changed} files edited, {files_deleted} files deleted")
    return 0


if __name__ == "__main__":
    sys.exit(main())
