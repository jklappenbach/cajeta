#!/usr/bin/env python3
"""Construct test/routine_filter.txt — the coverage-derived routine gate.

The routine gate is the everyday sweep (test-battery-restructure 2.5): the
minimal test set that holds the target share of the full battery's measured
line coverage, UNIONed with the curated pin manifests, so the fast gate keeps
both the coverage and the behaviour pins. The FULL battery stays available
(FULL=1 in cajeta_tests.sh); nothing is deleted.

Membership =
  1. greedy set cover of the measured per-test line coverage to --target
     (later --index dirs override earlier ones on unit-name collision), over
     units present in the CURRENT battery only;
  2. every current test named in a --pins manifest (regression/release pins:
     line-redundant by design — each is the only assertion of some value,
     ordering, or diagnostic);
  3. every current test with NO coverage measurement (can't be proven
     redundant, so it stays until measured).

Usage:
  build_routine.py --battery current-units.txt \
      --index .coverage/index --index .coverage/refold/index \
      --index .coverage/newmeas/index \
      --pins test/regression_filter.txt --pins test/release_filter.txt \
      --target 0.995 --out test/routine_filter.txt
"""
import argparse, json, os, re, sys


def load_units(index_dirs, battery):
    units = {}
    for d in index_dirs:
        for fn in os.listdir(d):
            if not fn.endswith('.json'):
                continue
            name = fn[:-5]
            if name not in battery:
                continue
            with open(os.path.join(d, fn)) as fh:
                units[name] = json.load(fh)['files']
    return units


def read_names(path):
    names = []
    with open(path) as fh:
        for line in fh:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            names.append(line)
    return names



def selftest():
    """Every rule here was written because its absence deleted real tests."""
    import subprocess, tempfile
    me = os.path.abspath(__file__)
    with tempfile.TemporaryDirectory() as t:
        idx = os.path.join(t, 'index'); os.makedirs(idx)
        def unit(n, files):
            json.dump({'unit': n, 'files': files},
                      open(os.path.join(idx, n + '.json'), 'w'))
        unit('A.covers',    {'/s/a.cpp': list(range(1, 60))})
        unit('B.redundant', {'/s/a.cpp': [1, 2]})
        unit('C.subproc',   {'/s/a.cpp': []})
        unit('D.nofiles',   {})
        unit('E.slowwide',  {'/s/b.cpp': list(range(1, 200))})
        bat = os.path.join(t, 'b.txt')
        open(bat, 'w').write('\n'.join(
            ['A.covers','B.redundant','C.subproc','D.nofiles','E.slowwide',
             'F.DISABLED_off']) + '\n')
        dur = os.path.join(t, 'd.tsv')
        open(dur, 'w').write('A.covers\t1000\nE.slowwide\t100000\n')
        out = os.path.join(t, 'o.txt')

        def run(*extra, pins=None):
            cmd = [sys.executable, me, '--battery', bat, '--index', idx,
                   '--out', out, *extra]
            if pins:
                pf = os.path.join(t, 'p.txt'); open(pf, 'w').write(pins)
                cmd += ['--pins', pf]
            r = subprocess.run(cmd, capture_output=True, text=True)
            got = set()
            if os.path.exists(out):
                got = {l.strip() for l in open(out)
                       if l.strip() and not l.startswith('#')}
            return r, got

        # Zero attribution is UNMEASURABLE, not redundant: it is the signature
        # of a subprocess test (and of a crash before gcov flushed).
        r, g = run('--target', '0.995')
        assert r.returncode == 0, r.stderr
        assert 'C.subproc' in g and 'D.nofiles' in g, g
        assert 'B.redundant' not in g, 'a genuinely redundant unit was kept'

        # A time budget must buy lines per SECOND, not raw gain.
        r, g = run('--budget-seconds', '5', '--durations', dur,
                   '--no-keep-unmeasured')
        assert 'A.covers' in g and 'E.slowwide' not in g, g

        # --exclude wins over every keep rule.
        exc = os.path.join(t, 'x.txt'); open(exc, 'w').write('C.subproc\n')
        r, g = run('--target', '0.995', '--exclude', exc)
        assert 'C.subproc' not in g, g

        # Suite level emits Suite.* and budgets per SUITE.
        r, g = run('--target', '0.995', '--suite-level')
        assert all(x.endswith('.*') for x in g), g

        # A pin naming no live test is FATAL: it protects nothing, silently,
        # and whatever replaced it is left deletable.
        r, _ = run('--target', '0.995', pins='A.renamedAway\n')
        assert r.returncode == 2, 'a dangling pin did not fail'
        assert 'A.renamedAway' in r.stderr, r.stderr
        r, _ = run('--target', '0.995', '--allow-dangling-pins',
                   pins='A.renamedAway\n')
        assert r.returncode == 0, r.stderr
        # A wildcard pin that MATCHES is not dangling.
        r, _ = run('--target', '0.995', pins='A.*\n')
        assert r.returncode == 0, r.stderr
        # A DISABLED pin is inert, not missing — warn, do not fail.
        r, _ = run('--target', '0.995', pins='F.DISABLED_off\n')
        assert r.returncode == 0, r.stderr
        assert 'DISABLED' in r.stderr and 'inert' in r.stderr, r.stderr
    print('selftest: ok')
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--selftest', action='store_true')
    ap.add_argument('--battery',
                    help='file listing every Suite.test in the current binary')
    ap.add_argument('--index', action='append',
                    help='per-test index dir; later dirs override earlier')
    ap.add_argument('--pins', action='append', default=[],
                    help='curated manifest whose tests always stay')
    ap.add_argument('--allow-dangling-pins', action='store_true',
                    help='downgrade unresolvable pins to a warning')
    ap.add_argument('--target', type=float, default=0.995)
    ap.add_argument('--exclude', action='append', default=[],
                    help='manifest whose tests are never emitted (stress: the '
                         'runner drops them anyway, so listing one here is a '
                         'lie about what the gate runs)')
    ap.add_argument('--budget-seconds', type=float,
                    help='derive a TIME-bounded set instead of a coverage '
                         'target: greedily take the best lines-per-second '
                         'until the budget is spent. This is what light_filter '
                         'needs — its contract is "~90s", which a coverage '
                         'target cannot express.')
    ap.add_argument('--durations', help='TSV: Suite.test<TAB>ms (for --budget-seconds)')
    ap.add_argument('--no-keep-unmeasured', action='store_true',
                    help="do NOT union unmeasured/unmeasurable tests into the "
                         "result. The keep rule exists because routine_filter "
                         "is used as a DELETE-SET COMPLEMENT — 'cannot be "
                         "proven redundant, so do not remove it'. A subset gate "
                         "(light/release) deletes nothing, so it buys no safety "
                         "there and only costs wall-clock: on 2026-08-16 it put "
                         "507 unmeasurable tests into light, blowing its ~90s "
                         "contract out to ~338s.")
    ap.add_argument('--suite-level', action='store_true',
                    help="emit 'Suite.*' per selected test's suite (release "
                         "filter's form: its rationale is per-suite host "
                         "independence, not per-test)")
    ap.add_argument('--out')
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if not (a.battery and a.index and a.out):
        ap.error('--battery, --index and --out are required')

    battery = set(read_names(a.battery))
    runnable = {u for u in battery if '.DISABLED_' not in u
                and not u.split('.', 1)[1].startswith('DISABLED_')}
    units = load_units(a.index, runnable)

    # Flatten to global line ids; big-int bitmaps for C-speed set ops.
    ids = {}
    masks = {}
    for u, files in units.items():
        m = 0
        for f, lines in files.items():
            for ln in lines:
                k = (f, ln)
                i = ids.setdefault(k, len(ids))
                m |= 1 << i
        masks[u] = m

    universe_bits = 0
    for m in masks.values():
        universe_bits |= m
    universe = universe_bits.bit_count()

    # --suite-level: the unit of selection is the SUITE, because that is what
    # release_filter names (`CompilerTests.*`) and what the release workflow
    # runs. Budgeting per TEST and collapsing afterwards is incoherent —
    # selecting one test drags in every sibling, so a 3,013s test-level budget
    # realised as 26,464s when first tried. Aggregate first: a suite's mask is
    # the union of its tests', its cost the sum.
    unit_suite = {}
    if a.suite_level:
        agg, agg_dur = {}, {}
        for u, m in masks.items():
            suite = u.split('.', 1)[0]
            agg[suite] = agg.get(suite, 0) | m
            unit_suite.setdefault(suite, []).append(u)
        masks = agg

    chosen = []
    got = 0
    pool = dict(masks)
    if a.budget_seconds:
        # Maximise coverage per SECOND, not per test. Picking by raw gain fills
        # the budget with whatever is biggest, which here is also the slowest —
        # SortJoinTests alone is 177s and would eat twice light's entire budget.
        dur = {}
        if a.durations:
            for line in open(a.durations):
                parts = line.rstrip('\n').split('\t')
                if len(parts) >= 2:
                    try:
                        dur[parts[0]] = int(parts[1]) / 1000.0
                    except ValueError:
                        pass
        if a.suite_level:
            dur = {suite: sum(dur.get(u, 1.5) for u in members)
                   for suite, members in unit_suite.items()}
        spent = 0.0
        while pool:
            best_u, best_rate, best_gain = None, -1.0, 0
            for u, m in pool.items():
                cost = max(dur.get(u, 1.5), 0.001)
                if spent + cost > a.budget_seconds:
                    continue
                gain = (m & ~got).bit_count()
                rate = gain / cost
                if rate > best_rate:
                    best_u, best_rate, best_gain = u, rate, gain
            if best_u is None or best_gain <= 0:
                break
            chosen.append(best_u)
            spent += max(dur.get(best_u, 1.5), 0.001)
            got |= pool.pop(best_u)
    else:
        goal = int(universe * a.target)
        while got.bit_count() < goal and pool:
            best_u, best_gain = None, -1
            for u, m in pool.items():
                gain = (m & ~got).bit_count()
                if gain > best_gain:
                    best_u, best_gain = u, gain
            if best_gain <= 0:
                break
            chosen.append(best_u)
            got |= pool.pop(best_u)

    cover = set(chosen)
    pinned = set()
    dangling_pins = []
    disabled_pins = []
    for p in a.pins:
        for name in read_names(p):
            if name in runnable:
                pinned.add(name)
            elif '*' in name or '?' in name:
                rx = re.compile('^' + re.escape(name)
                                .replace(r'\*', '.*').replace(r'\?', '.') + '$')
                hit = {u for u in runnable if rx.match(u)}
                if hit:
                    pinned |= hit
                else:
                    dangling_pins.append((p, name))
            elif name in battery:
                # Exists, but gtest never runs it. Pinning a DISABLED test is a
                # different defect from pinning a renamed one: nothing was lost,
                # the pin is simply inert. Say which, or the fix looks like a
                # rename hunt that will find nothing.
                disabled_pins.append((p, name))
            else:
                dangling_pins.append((p, name))

    # A DANGLING PIN PROTECTS NOTHING, SILENTLY. This used to be `if name in
    # runnable` with no else: a pin whose test had been renamed was skipped
    # without a word, so the behaviour it named went unprotected and the test
    # that actually holds the assertion became deletable. On 2026-08-16 that
    # deleted the successors of three of the four dangling pins in
    # regression_filter.txt — the manifest said "protect this", the name was one
    # rename out of date, and the tool agreed by saying nothing.
    if disabled_pins:
        sys.stderr.write(
            f'WARNING: {len(disabled_pins)} pin(s) name a DISABLED test — it is '
            'in the binary but gtest never runs it, so the pin is inert:\n')
        for src, name in disabled_pins:
            sys.stderr.write(f'    {name}  ({src})\n')
    if dangling_pins:
        sys.stderr.write(
            f'ERROR: {len(dangling_pins)} pin(s) name no live test. A pin that '
            'resolves to nothing protects nothing, and whatever replaced it is '
            'unprotected:\n')
        for src, name in dangling_pins:
            sys.stderr.write(f'    {name}  ({src})\n')
        sys.stderr.write('Repair the manifest (map each to its successor) or '
                         'pass --allow-dangling-pins.\n')
        if not a.allow_dangling_pins:
            return 2

    # UNMEASURABLE, not redundant. A unit whose index entry attributes ZERO
    # lines ran and covered nothing the parent process could see — the
    # signature of a test that drives the `cajeta` CLI as a child process
    # (ANALYSIS caveat 2). gcov instruments the parent; the work happens in the
    # fork. Treating that as "covers nothing, therefore droppable" deletes the
    # only tests of the entire CLI surface, which is what happened on
    # 2026-08-15: 227 such tests went, including every BuildToolArms and
    # ArchiveCommand case.
    #
    # "Can't be proven redundant, so it stays" already governs tests with no
    # entry at all. A zero-line entry is the same epistemic position with more
    # steps, so it gets the same answer.
    #
    # NOTE the conflation, which is deliberate: zero attribution also means "the
    # test CRASHED before gcov could flush" (index_gcov.py says so where it
    # records the empty unit). This rule therefore promotes known-crashing tests
    # into the gate, which is how
    # SharedStdlibDylibSpike.sharedStdlibResolvesAcrossUserDylibsCtorRunsOnce
    # was found on 2026-08-15 — crashing since at least the 2026-08-10 measure,
    # outside the gate, so the sweep never ran it and nobody knew. Keeping a
    # crasher visible is the right answer; a test that cannot complete is the
    # last thing that should be silently deleted for "covering nothing".
    unmeasurable = {u for u, files in units.items()
                    if not any(files.values())}
    unmeasured = (runnable - set(units)) | unmeasurable
    if a.no_keep_unmeasured:
        unmeasured = set()

    excluded = set()
    for e in a.exclude:
        for name in read_names(e):
            excluded.add(name)
    if a.suite_level:
        excl_suites = {e.split('.', 1)[0] for e in excluded}
        gate = sorted({c + '.*' for c in cover
                       if c.split('.', 1)[0] not in excl_suites})
    else:
        gate = sorted((cover | pinned | unmeasured) - excluded)

    held = got
    for u in gate:
        key = u[:-2] if a.suite_level and u.endswith('.*') else u
        if key in masks:
            held |= masks[key]

    if a.budget_seconds:
        how = (f"#   time-budgeted cover: best lines/second within "
               f"{a.budget_seconds:.0f}s ({len(cover)} tests)\n")
    else:
        how = (f"#   greedy {a.target:.1%} line-coverage cover"
               f" ({len(cover)} tests)\n")

    header = [
        "# Coverage-derived gate (test-battery-restructure 2.5 / 8.2).\n",
        "#\n",
        "# Generated by tools/coverage/build_routine.py; do not hand-edit the list —\n",
        "# regenerate after the next per-test coverage measure. Membership:\n",
        how,
        f"#   + curated pins ({len(pinned - cover)} additional)\n",
        f"#   + unmeasured/unmeasurable tests "
        f"({len(unmeasured - cover - pinned)} additional, of which "
        f"{len(unmeasurable - cover - pinned)} attribute zero lines because "
        "their work happens in a child process)\n",
    ]
    if excluded:
        header.append(
            f"#   - excluded by manifest ({len(excluded)} names — the stress "
            "battery, which the runner drops anyway, so listing one here would\n"
            "#     be a lie about what this gate runs)\n")
    if a.suite_level:
        header.append("#   collapsed to Suite.* form (per-suite rationale)\n")
    header += [
        f"# Coverage held: {held.bit_count()}/{universe} measured lines"
        f" = {held.bit_count()/max(1,universe):.2%}\n",
        f"# Battery: {len(runnable)} runnable tests -> gate keeps {len(gate)}"
        f" ({len(gate)/max(1,len(runnable)):.0%})\n",
    ]
    with open(a.out, 'w') as fh:
        fh.writelines(header)
        for u in gate:
            fh.write(u + '\n')

    print(f'battery(runnable): {len(runnable)}')
    print(f'measured:          {len(units)}')
    print(f'universe lines:    {universe}')
    print(f'cover:             {len(cover)} tests -> '
          f'{got.bit_count()}/{universe} = '
          f'{got.bit_count()/max(1,universe):.2%}')
    print(f'pins added:        {len(pinned - cover)}')
    print(f'unmeasured kept:   {len(unmeasured - cover - pinned)} '
          f'(of which {len(unmeasurable - cover - pinned)} measured-as-zero: '
          f'subprocess blind spot)')
    print(f'GATE TOTAL:        {len(gate)} '
          f'({len(gate)/max(1,len(runnable)):.0%} of battery), '
          f'holding {held.bit_count()/max(1,universe):.2%} of measured lines')
    print(f'wrote {a.out}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
