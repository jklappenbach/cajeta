#!/usr/bin/env python3
"""Live, per-shard curses front-end for the Cajeta gtest suite.

Why this exists
---------------
`run_tests.sh` shards the ~3300-test suite across cores but only prints a
summary *after* every shard finishes — with 3000+ tests and slow JIT
compiles you stare at a blank screen for minutes. This driver shows, live:

  * an overall completion bar (tests done / total), and
  * one row per shard: the test it is running right now, that shard's
    completion bar, and how long the current test has been going.

gtest emits no progress *inside* a single test, so a true per-test
percentage is impossible; per row we show the shard's % (tests done /
assigned) plus the current test's elapsed time. A test running unusually
long is flagged so a hang is visible at a glance.

It is also the structured-results source for the flakiness work: every run
writes a JSON record (per-test status + duration, per-shard exit) via
--results, which aggregates cleanly across repeated runs — far better than
scraping --gtest_brief logs after the fact.

Headless / CI
-------------
With no controlling tty (e.g. launched in the background) or with
--no-tui, it falls back to periodic plain-text progress and still writes
the JSON. Same orchestration core, two front-ends.

Usage
-----
    scripts/test-tui.py                       # whole suite, all cores
    scripts/test-tui.py 'Net*' 'Tls*'         # gtest filter patterns
    scripts/test-tui.py --shards 16
    scripts/test-tui.py --results /tmp/r.json --no-tui

Exit code is non-zero if any test failed or any shard crashed/hung.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import select
import shutil
import subprocess
import sys
import time

# --------------------------------------------------------------------------
# gtest line grammar. Brackets carry fixed-width padding; allow any run of
# spaces so we are robust to gtest version drift. We only treat OK/FAILED as
# a *test result* when the token looks like Suite.test (contains a dot) —
# this skips the trailing "[  FAILED  ] N tests, listed below:" summary line.
# --------------------------------------------------------------------------
RUN_RE = re.compile(r"^\[\s*RUN\s*\]\s+(\S+)")
OK_RE = re.compile(r"^\[\s*OK\s*\]\s+(\S+?)\s+\((\d+)\s*ms\)")
FAIL_RE = re.compile(r"^\[\s*FAILED\s*\]\s+(\S+?)\s+\((\d+)\s*ms\)")

# A test that has been running longer than this (seconds) is flagged in the
# UI as possibly stuck. Purely cosmetic; does not affect the kill timeout.
LONG_TEST_S = 20.0


def repo_root() -> str:
    # scripts/ lives directly under the repo root.
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def discover_tests(bin_path: str, root: str, patterns: list[str],
                   exclude: list[str] | None = None) -> list[str]:
    """Enumerate Suite.test names via --gtest_list_tests, honoring filters.

    `exclude` patterns become gtest's negative clause (after `-`), so
    quarantined tests are never sharded — handy for measuring the suite
    minus a known-poison set that otherwise crashes shards early.
    """
    cmd = [bin_path, "--gtest_list_tests"]
    pos = ":".join(_expand(p) for p in patterns) if patterns else ""
    neg = ":".join(_expand(p) for p in exclude) if exclude else ""
    if pos or neg:
        cmd.append("--gtest_filter=" + (pos or "*") + ("-" + neg if neg else ""))
    env = dict(os.environ, CAJETA_SOURCE_ROOT=root)
    out = subprocess.run(
        cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
        text=True,
    ).stdout
    tests: list[str] = []
    suite = ""
    for line in out.splitlines():
        m = re.match(r"^([A-Za-z_][A-Za-z0-9_]*)\.\s*$", line)
        if m:
            suite = m.group(1)
            continue
        m = re.match(r"^\s+([A-Za-z_][A-Za-z0-9_/]*)", line)
        if m and suite:
            tests.append(f"{suite}.{m.group(1)}")
    return tests


def _expand(pat: str) -> str:
    # Bare suite name -> whole suite, matching run_tests.sh ergonomics.
    if "." not in pat and "*" not in pat:
        return pat + ".*"
    return pat


class Shard:
    """One worker process running a round-robin slice of the test list."""

    def __init__(self, sid: int, tests: list[str]):
        self.id = sid
        self.tests = tests
        self.assigned = len(tests)
        self.done = 0
        self.passed = 0
        self.failed = 0
        self.current = ""           # test currently running ("" = between/idle)
        self.current_start = 0.0
        self.status = "pending"      # pending|running|done|crashed|hung
        self.exit_code: int | None = None
        self.results: dict[str, dict] = {}   # test -> {status, ms}
        self.proc: subprocess.Popen | None = None
        self.fd = -1
        self.buf = b""

    # -- lifecycle ---------------------------------------------------------
    def spawn(self, bin_path: str, root: str, timeout_s: int, kill_after: int):
        if not self.tests:
            self.status = "done"
            return
        cmd = [
            "timeout", f"--kill-after={kill_after}", str(timeout_s),
            bin_path,
            "--gtest_filter=" + ":".join(self.tests),
            "--gtest_color=no",
        ]
        env = dict(os.environ, CAJETA_SOURCE_ROOT=root)
        self.proc = subprocess.Popen(
            cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            bufsize=0,
        )
        self.fd = self.proc.stdout.fileno()
        os.set_blocking(self.fd, False)
        self.status = "running"

    def feed(self, now: float):
        """Drain whatever bytes are ready; update state from gtest lines."""
        try:
            chunk = os.read(self.fd, 65536)
        except (BlockingIOError, OSError):
            return
        if not chunk:                # EOF — process is finishing
            self._finish(now)
            return
        self.buf += chunk
        *lines, self.buf = self.buf.split(b"\n")
        for raw in lines:
            self._line(raw.decode("utf-8", "replace"), now)

    def _line(self, line: str, now: float):
        m = RUN_RE.match(line)
        if m:
            self.current = m.group(1)
            self.current_start = now
            return
        m = OK_RE.match(line)
        if m and "." in m.group(1):
            self._record(m.group(1), "pass", int(m.group(2)))
            return
        m = FAIL_RE.match(line)
        if m and "." in m.group(1):
            self._record(m.group(1), "fail", int(m.group(2)))
            self.failed += 1

    def _record(self, name: str, status: str, ms: int):
        if name not in self.results:
            self.done += 1
        self.results[name] = {"status": status, "ms": ms, "shard": self.id}
        if status == "pass":
            self.passed += 1
        self.current = ""

    def _finish(self, now: float):
        if self.status in ("done", "crashed", "hung"):
            return
        self.proc.wait()
        self.exit_code = self.proc.returncode
        # How long the mid-flight test had been running when the shard ended.
        # This separates a *genuine* single-test hang (ran tens of seconds)
        # from a slow shard merely killed by the wall-clock with some normal
        # test unluckily mid-flight (just started — small elapsed).
        killed_ms = int(max(0.0, now - self.current_start) * 1000) \
            if self.current else 0
        # timeout(1): 124 = SIGTERM at deadline, 137 = SIGKILL after grace.
        if self.exit_code in (124, 137):
            self.status = "hung"
            self._mark_unfinished("hung", killed_ms)
        elif self.exit_code not in (0, 1):
            # Non-zero outside gtest's own "1 = some test failed": a crash
            # (segfault/abort). Blame the test that was mid-flight.
            self.status = "crashed"
            self._mark_unfinished("crash", killed_ms)
        else:
            self.status = "done"
            self._mark_unfinished("notrun", 0)

    def _mark_unfinished(self, kind: str, killed_ms: int):
        """Tests in this shard that never produced a result line."""
        if self.current and self.current not in self.results:
            self.results[self.current] = {
                "status": kind, "ms": killed_ms, "shard": self.id}
            self.done += 1
            if kind in ("crash", "hung"):
                self.failed += 1
        for t in self.tests:
            if t not in self.results:
                self.results[t] = {"status": "notrun", "ms": 0,
                                   "shard": self.id}
        self.current = ""

    @property
    def pct(self) -> float:
        return 100.0 * self.done / self.assigned if self.assigned else 100.0


def partition(tests: list[str], n: int) -> list[list[str]]:
    """Round-robin, identical to run_tests.sh so shard identity is stable."""
    buckets: list[list[str]] = [[] for _ in range(n)]
    for i, t in enumerate(tests):
        buckets[i % n].append(t)
    return buckets


# ==========================================================================
# Isolate mode: one fresh process per test.
#
# Measured fact that motivates this: the ~8s stdlib JIT recompile is paid
# per *test* even inside a shared process (zero amortization), while a
# fork/exec of the 512MB binary is ~22ms. So a process-per-test costs the
# same wall-clock as sharding but gives true isolation — a crash or hang
# loses exactly one test (no shard wipes out ~100 siblings, no quarantine),
# and process-global state (e.g. CajetaModule's static structure map) can't
# leak between tests. A lane is a worker that pulls the next test, runs it
# alone, records the result, and repeats.
# ==========================================================================
class Lane:
    def __init__(self, lid: int):
        self.id = lid
        self.test = ""              # test currently running ("" = idle)
        self.current = ""           # alias used by the shared renderer
        self.current_start = 0.0
        self.done = 0               # tests this lane has finished
        self.passed = 0
        self.failed = 0
        self.status = "idle"        # idle|running|finished
        self.proc: subprocess.Popen | None = None
        self.fd = -1
        self.buf = b""
        self.saw_fail = False

    def start(self, test: str, bin_path: str, root: str,
              timeout_s: int, kill_after: int, now: float):
        self.test = self.current = test
        self.current_start = now
        self.buf = b""
        self.saw_fail = False
        cmd = ["timeout", f"--kill-after={kill_after}", str(timeout_s),
               bin_path, f"--gtest_filter={test}", "--gtest_color=no"]
        env = dict(os.environ, CAJETA_SOURCE_ROOT=root)
        self.proc = subprocess.Popen(
            cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            bufsize=0)
        self.fd = self.proc.stdout.fileno()
        os.set_blocking(self.fd, False)
        self.status = "running"

    def feed(self, now: float) -> dict | None:
        """Drain output; return a result dict once the test's process ends."""
        try:
            chunk = os.read(self.fd, 65536)
        except (BlockingIOError, OSError):
            return None
        if chunk:
            self.buf += chunk
            for raw in self.buf.split(b"\n"):
                line = raw.decode("utf-8", "replace")
                m = FAIL_RE.match(line)
                if m and "." in m.group(1):
                    self.saw_fail = True
            return None
        return self._finish(now)        # EOF

    def _finish(self, now: float) -> dict:
        self.proc.wait()
        code = self.proc.returncode
        ms = int(max(0.0, now - self.current_start) * 1000)
        if code in (124, 137):
            status = "hung"
        elif self.saw_fail or code == 1:
            status = "fail"
        elif code != 0:
            status = "crash"
        else:
            status = "pass"
        self.done += 1
        if status == "pass":
            self.passed += 1
        else:
            self.failed += 1
        res = {"status": status, "ms": ms, "lane": self.id, "test": self.test,
               "start_epoch": self.current_start}
        self.status = "idle"
        self.test = self.current = ""
        self.fd = -1
        self.proc = None
        return res


def run_isolate(args, bin_path, root, tests) -> int:
    nlanes = args.shards or min(32, os.cpu_count() or 4)
    nlanes = min(nlanes, len(tests))
    lanes = [Lane(i) for i in range(nlanes)]
    queue = list(tests)
    qi = 0
    results: dict[str, dict] = {}
    started = time.time()

    def launch(ln: Lane, now: float):
        nonlocal qi
        if qi < len(queue):
            ln.start(queue[qi], bin_path, root, args.test_timeout,
                     args.kill_after, now)
            qi += 1

    for ln in lanes:
        launch(ln, started)

    use_tui = (not args.no_tui) and sys.stdout.isatty()

    def record(res):
        results[res["test"]] = res
        if not use_tui:
            # Stream one line per test as it finishes (the user wants live
            # per-test progress, with each test's start clock time and the
            # overall run %). A hung test still lands here as HUNG after its
            # per-test timeout, so every test produces exactly one line.
            print(_stream_line(res, len(results), len(tests)), flush=True)

    def loop(scr):
        nonlocal qi
        if scr is not None:
            import curses
            curses.curs_set(0)
            scr.nodelay(True)
        while any(ln.status == "running" for ln in lanes) or qi < len(queue):
            now = time.time()
            fds = [ln.fd for ln in lanes if ln.status == "running"
                   and ln.fd >= 0]
            if fds:
                r, _, _ = select.select(fds, [], [], 0.12)
                ready = set(r)
                for ln in lanes:
                    if ln.status == "running" and ln.fd in ready:
                        res = ln.feed(now)
                        if res:
                            record(res)
                            launch(ln, time.time())
                # Reap any whose process exited without select firing on EOF.
                for ln in lanes:
                    if ln.status == "running" and ln.proc.poll() is not None:
                        res = ln.feed(time.time())
                        if res:
                            record(res)
                            launch(ln, time.time())
            else:
                time.sleep(0.05)
            if scr is not None:
                _draw_isolate(scr, lanes, results, len(tests),
                              now - started)

    try:
        if use_tui:
            import curses
            curses.wrapper(loop)
        else:
            loop(None)
    except KeyboardInterrupt:
        for ln in lanes:
            if ln.proc and ln.proc.poll() is None:
                ln.proc.kill()
        sys.stderr.write("\ninterrupted — killed lanes\n")

    return _report_isolate(results, tests, nlanes, time.time() - started,
                           started, args.results)


def run(args) -> int:
    root = args.root or os.environ.get("CAJETA_SOURCE_ROOT") or repo_root()
    bin_path = args.bin or os.path.join(root, "build", "test", "cajeta_test")
    if not os.access(bin_path, os.X_OK):
        sys.stderr.write(f"error: test binary not executable: {bin_path}\n")
        return 2

    exclude = [p for clause in args.exclude for p in clause.split(":") if p]
    tests = discover_tests(bin_path, root, args.patterns, exclude)
    if not tests:
        sys.stderr.write("error: no tests discovered\n")
        return 2

    if args.isolate:
        return run_isolate(args, bin_path, root, tests)

    nshards = args.shards or min(32, os.cpu_count() or 4)
    nshards = min(nshards, len(tests))
    shards = [Shard(i, b) for i, b in enumerate(partition(tests, nshards))]

    started = time.time()
    for sh in shards:
        sh.spawn(bin_path, root, args.shard_timeout, args.kill_after)

    use_tui = (not args.no_tui) and sys.stdout.isatty()
    try:
        if use_tui:
            import curses
            curses.wrapper(lambda scr: _loop(scr, shards, len(tests), started))
        else:
            _loop(None, shards, len(tests), started, plain=True)
    except KeyboardInterrupt:
        for sh in shards:
            if sh.proc and sh.proc.poll() is None:
                sh.proc.kill()
        sys.stderr.write("\ninterrupted — killed shards\n")

    elapsed = time.time() - started
    return _report(shards, tests, nshards, elapsed, started, args.results)


def _alive(shards: list[Shard]) -> bool:
    return any(sh.status in ("pending", "running") for sh in shards)


def _loop(scr, shards, total, started, plain=False):
    """Single select() loop drives both front-ends off the same state."""
    last_plain = 0.0
    if scr is not None:
        import curses
        curses.curs_set(0)
        scr.nodelay(True)
    while _alive(shards):
        now = time.time()
        fds = [sh.fd for sh in shards if sh.status == "running" and sh.fd >= 0]
        if fds:
            r, _, _ = select.select(fds, [], [], 0.12)
            ready = set(r)
            for sh in shards:
                if sh.status == "running" and sh.fd in ready:
                    sh.feed(now)
            # Reap processes that closed without us seeing EOF via select.
            for sh in shards:
                if sh.status == "running" and sh.proc.poll() is not None:
                    sh.feed(time.time())
                    if sh.status == "running":
                        sh._finish(time.time())
        else:
            time.sleep(0.1)
        if scr is not None:
            _draw(scr, shards, total, now - started)
        elif now - last_plain >= 3.0:
            _plain(shards, total, now - started)
            last_plain = now
    # Final repaint so the last test's completion is reflected.
    if scr is not None:
        _draw(scr, shards, total, time.time() - started)


# --------------------------------------------------------------------------
# Rendering
# --------------------------------------------------------------------------
def _totals(shards):
    done = sum(s.done for s in shards)
    passed = sum(s.passed for s in shards)
    failed = sum(s.failed for s in shards)
    return done, passed, failed


def _bar(pct: float, width: int) -> str:
    fill = int(round(width * pct / 100.0))
    return "#" * fill + "-" * (width - fill)


def _fmt_elapsed(s: float) -> str:
    return f"{int(s) // 60:02d}:{int(s) % 60:02d}"


def _draw(scr, shards, total, elapsed):
    import curses
    scr.erase()
    rows, cols = scr.getmaxyx()
    done, passed, failed = _totals(shards)
    pct = 100.0 * done / total if total else 100.0

    head = (f"Cajeta Tests   {done}/{total}  {pct:4.0f}%  "
            f"[{_bar(pct, 20)}]  {failed} failed  {_fmt_elapsed(elapsed)}")
    scr.addnstr(0, 0, head, cols - 1, curses.A_BOLD)
    scr.addnstr(1, 0, "-" * (cols - 1), cols - 1)

    # Running shards first (most interesting), then the rest.
    order = sorted(shards, key=lambda s: (s.status != "running", s.id))
    body_rows = max(0, rows - 4)
    now = time.time()
    shown = 0
    for sh in order:
        if shown >= body_rows:
            break
        line, attr = _shard_line(sh, now, cols)
        scr.addnstr(2 + shown, 0, line, cols - 1, attr)
        shown += 1
    if shown < len(order):
        scr.addnstr(2 + shown, 0,
                    f"... (+{len(order) - shown} more shards)", cols - 1)

    scr.refresh()


def _shard_line(sh, now, cols):
    import curses
    tag = f"sh{sh.id:02d}"
    if sh.status == "running":
        cur = sh.current or "(starting)"
        el = now - sh.current_start if sh.current else 0.0
        mark = " *" if el >= LONG_TEST_S else ""
        # Leave room for fixed columns; truncate the test name.
        name_w = max(10, cols - 34)
        line = (f"{tag} {sh.pct:3.0f}% [{_bar(sh.pct, 10)}] "
                f"{cur[:name_w]:<{name_w}} {el:5.1f}s{mark}")
        return line, curses.A_NORMAL
    if sh.status == "hung":
        return f"{tag} {sh.pct:3.0f}% [{_bar(sh.pct, 10)}] HUNG (timeout)", \
            curses.A_BOLD
    if sh.status == "crashed":
        return (f"{tag} {sh.pct:3.0f}% [{_bar(sh.pct, 10)}] "
                f"CRASH exit={sh.exit_code}"), curses.A_BOLD
    if sh.status == "done":
        flag = f"  {sh.failed} FAIL" if sh.failed else ""
        return (f"{tag} 100% [{_bar(100, 10)}] done  "
                f"{sh.passed} ok{flag}"), curses.A_DIM
    return f"{tag}   0% [{_bar(0, 10)}] pending", curses.A_DIM


def _plain(shards, total, elapsed):
    done, passed, failed = _totals(shards)
    pct = 100.0 * done / total if total else 100.0
    running = sum(1 for s in shards if s.status == "running")
    print(f"[{_fmt_elapsed(elapsed)}] {done}/{total} ({pct:4.1f}%)  "
          f"pass={passed} fail={failed}  shards running={running}",
          flush=True)


# --- isolate-mode rendering ------------------------------------------------
def _iso_totals(results):
    done = len(results)
    passed = sum(1 for r in results.values() if r["status"] == "pass")
    return done, passed, done - passed


def _draw_isolate(scr, lanes, results, total, elapsed):
    import curses
    scr.erase()
    rows, cols = scr.getmaxyx()
    done, passed, failed = _iso_totals(results)
    pct = 100.0 * done / total if total else 100.0
    head = (f"Cajeta Tests [isolate]  {done}/{total}  {pct:4.0f}%  "
            f"[{_bar(pct, 20)}]  {failed} failed  {_fmt_elapsed(elapsed)}")
    scr.addnstr(0, 0, head, cols - 1, curses.A_BOLD)
    scr.addnstr(1, 0, "-" * (cols - 1), cols - 1)
    now = time.time()
    order = sorted(lanes, key=lambda L: (L.status != "running", L.id))
    body = max(0, rows - 4)
    for i, ln in enumerate(order[:body]):
        scr.addnstr(2 + i, 0, _lane_line(ln, now, cols), cols - 1,
                    curses.A_NORMAL if ln.status == "running" else curses.A_DIM)
    scr.refresh()


def _lane_line(ln, now, cols):
    tag = f"lane{ln.id:02d}"
    if ln.status == "running":
        el = now - ln.current_start
        mark = " *" if el >= LONG_TEST_S else ""
        start = time.strftime("%H:%M:%S", time.localtime(ln.current_start))
        name_w = max(10, cols - 42)
        return (f"{tag} {start} {ln.current[:name_w]:<{name_w}} "
                f"{el:5.1f}s{mark}  ({ln.done} done)")
    return f"{tag} (idle)  {ln.done} done  {ln.passed} ok  {ln.failed} bad"


def _stream_line(res, done, total):
    """One per-test progress line: now-clock, overall %, status, name,
    duration, and the wall-clock time the test was started."""
    pct = 100.0 * done / total if total else 100.0
    start = time.strftime("%H:%M:%S", time.localtime(res["start_epoch"]))
    nowc = time.strftime("%H:%M:%S")
    status = res["status"].upper()
    return (f"{nowc}  {done:>4}/{total} {pct:5.1f}%  {status:<5}  "
            f"{res['test']:<52} {res['ms'] / 1000:6.1f}s  start {start}")


def _plain_isolate(lanes, results, total, elapsed):
    done, passed, failed = _iso_totals(results)
    pct = 100.0 * done / total if total else 100.0
    running = sum(1 for L in lanes if L.status == "running")
    print(f"[{_fmt_elapsed(elapsed)}] {done}/{total} ({pct:4.1f}%)  "
          f"pass={passed} fail={failed}  lanes running={running}", flush=True)


def _report_isolate(results, tests, nlanes, elapsed, started, results_path):
    from collections import Counter
    c = Counter(r["status"] for r in results.values())
    notrun = [t for t in tests if t not in results]
    bad = sorted((r["status"], n) for n, r in results.items()
                 if r["status"] != "pass")
    print()
    print("=== Test summary (isolate: one process per test) ===")
    print(f"Discovered: {len(tests)}   Lanes: {nlanes}   "
          f"Elapsed: {int(elapsed)}s")
    print(f"Passed: {c.get('pass', 0)}   Failed: {c.get('fail', 0)}   "
          f"Crashed: {c.get('crash', 0)}   Hung: {c.get('hung', 0)}   "
          f"Notrun: {len(notrun)}")
    if bad:
        print("\nFailing / crashing / hanging tests:")
        for status, name in bad:
            ms = results[name]["ms"]
            print(f"  [{status:5}] {name}  ({ms/1000:.1f}s)")
    if results_path:
        record = {
            "mode": "isolate", "started": started,
            "elapsed_s": round(elapsed, 1), "lanes": nlanes,
            "total": len(tests), "passed": c.get("pass", 0),
            "failed": c.get("fail", 0) + c.get("crash", 0) + c.get("hung", 0),
            "tests": results, "notrun": notrun,
        }
        os.makedirs(os.path.dirname(os.path.abspath(results_path)),
                    exist_ok=True)
        with open(results_path, "w") as f:
            json.dump(record, f, indent=2)
        print(f"\nstructured results -> {results_path}")
    return 1 if bad else 0


# --------------------------------------------------------------------------
# Final report + structured results
# --------------------------------------------------------------------------
def _report(shards, tests, nshards, elapsed, started, results_path) -> int:
    done, passed, failed = _totals(shards)
    crashed = [s for s in shards if s.status == "crashed"]
    hung = [s for s in shards if s.status == "hung"]

    fails = []
    for sh in shards:
        for name, r in sh.results.items():
            if r["status"] in ("fail", "crash", "hung"):
                fails.append((name, r["status"], sh.id))

    print()
    print("=== Test summary ===")
    print(f"Discovered: {len(tests)}   Shards: {nshards}   "
          f"Elapsed: {int(elapsed)}s")
    print(f"Passed: {passed}   Failed: {failed}   "
          f"Crashed shards: {len(crashed)}   Hung shards: {len(hung)}")
    if fails:
        print("\nFailing / crashing / hanging tests:")
        for name, status, sid in sorted(fails):
            print(f"  [{status:5}] {name}  (shard {sid})")

    if results_path:
        record = {
            "started": started,
            "elapsed_s": round(elapsed, 1),
            "shards": nshards,
            "total": len(tests),
            "passed": passed,
            "failed": failed,
            "crashed_shards": [s.id for s in crashed],
            "hung_shards": [s.id for s in hung],
            "tests": {name: r for sh in shards
                      for name, r in sh.results.items()},
            "shard_exit": {s.id: s.exit_code for s in shards},
        }
        os.makedirs(os.path.dirname(os.path.abspath(results_path)),
                    exist_ok=True)
        with open(results_path, "w") as f:
            json.dump(record, f, indent=2)
        print(f"\nstructured results -> {results_path}")

    return 1 if (failed or crashed or hung) else 0


def main() -> int:
    if not shutil.which("timeout"):
        sys.stderr.write("error: GNU 'timeout' not found on PATH\n")
        return 2
    p = argparse.ArgumentParser(description="Per-shard curses runner for the "
                                            "Cajeta gtest suite.")
    p.add_argument("patterns", nargs="*",
                   help="gtest filter patterns (bare suite name = whole suite)")
    p.add_argument("--shards", type=int, default=0,
                   help="shard count (default: min(32, cpu count))")
    p.add_argument("--isolate", action="store_true",
                   help="one fresh process per test: full crash/hang/state "
                        "isolation, no quarantine needed (same wall-clock as "
                        "sharding — the stdlib JIT cost is per-test regardless)")
    p.add_argument("--shard-timeout", type=int, default=300,
                   help="per-shard wall-clock timeout in seconds (default 300)")
    p.add_argument("--test-timeout", type=int, default=120,
                   help="isolate mode: per-test timeout in seconds (default "
                        "120 — a few golden-vector tests do ~8 stdlib JIT "
                        "compiles and legitimately run 45-65s; the durable fix "
                        "is to compile those once, see test/TEST_REPORT.md)")
    p.add_argument("--kill-after", type=int, default=10,
                   help="grace seconds before SIGKILL after timeout (default 10)")
    p.add_argument("--bin", default="",
                   help="path to cajeta_test (default build/test/cajeta_test)")
    p.add_argument("--root", default="",
                   help="CAJETA_SOURCE_ROOT (default: repo root)")
    p.add_argument("--results", default="",
                   help="write structured JSON results to this path")
    p.add_argument("--exclude", action="append", default=[],
                   help="quarantine: gtest patterns to skip (colon-separated "
                        "or repeat the flag). Bare suite name = whole suite.")
    p.add_argument("--no-tui", action="store_true",
                   help="force plain text output (auto when not a tty)")
    return run(p.parse_args())


if __name__ == "__main__":
    sys.exit(main())
