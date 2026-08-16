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
import argparse, json, os, sys


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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--battery', required=True,
                    help='file listing every Suite.test in the current binary')
    ap.add_argument('--index', action='append', required=True,
                    help='per-test index dir; later dirs override earlier')
    ap.add_argument('--pins', action='append', default=[],
                    help='curated manifest whose tests always stay')
    ap.add_argument('--target', type=float, default=0.995)
    ap.add_argument('--out', required=True)
    a = ap.parse_args()

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

    goal = int(universe * a.target)
    chosen = []
    got = 0
    pool = dict(masks)
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
    for p in a.pins:
        for name in read_names(p):
            if name in runnable:
                pinned.add(name)

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

    gate = sorted(cover | pinned | unmeasured)

    held = got
    for u in gate:
        if u in masks:
            held |= masks[u]

    with open(a.out, 'w') as fh:
        fh.write(
            "# Routine gate — coverage-derived everyday sweep "
            "(test-battery-restructure 2.5).\n"
            "#\n"
            "# Generated by tools/coverage/build_routine.py; do not hand-edit"
            " the list —\n"
            "# regenerate after the next full per-test coverage measure."
            " Membership:\n"
            f"#   greedy {a.target:.1%} line-coverage cover"
            f" ({len(cover)} tests)\n"
            f"#   + curated pins ({len(pinned - cover)} additional)\n"
            f"#   + unmeasured/unmeasurable tests "
            f"({len(unmeasured - cover - pinned)} additional, of which "
            f"{len(unmeasurable - cover - pinned)} attribute zero lines "
            "because their work happens in a child process)\n"
            f"# Coverage held: {held.bit_count()}/{universe} measured lines"
            f" = {held.bit_count()/max(1,universe):.2%}\n"
            f"# Battery: {len(runnable)} runnable tests -> gate keeps"
            f" {len(gate)} ({len(gate)/max(1,len(runnable)):.0%})\n"
            "#\n"
            "# The FULL battery still exists: FULL=1 ./cajeta_tests.sh\n")
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
