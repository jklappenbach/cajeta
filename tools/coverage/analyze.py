#!/usr/bin/env python3
"""Answer the three questions that shrink a test battery.

  report    per-file coverage, worst first — where is the compiler unexercised?
  optimize  which units are redundant, and what is the minimal covering subset?
  gaps      which files/functions are covered by NOTHING?

All three are set operations over the per-unit covered-line sets that
index_gcov.py produced.

On `optimize`: the minimal-subset problem is set cover, which is NP-hard, so
this uses the standard greedy approximation (repeatedly take the unit adding
the most new lines). Greedy is within a ln(n) factor of optimal — good enough to
act on, and it is reported as an approximation rather than "the" minimum.

IMPORTANT — what this cannot see. Line coverage is not behaviour. Two tests
covering identical lines can assert different things, and a test that adds zero
new lines may still be the only one pinning a value, an ordering, or an error
message. Treat "redundant" as a CANDIDATE list for human review, never as a
delete list. The `--keep-asserting` heuristic below is a partial guard, not a
substitute for reading the test.
"""
import argparse, json, os, re, sys
from collections import defaultdict


def load(data_dir):
    idx = os.path.join(data_dir, 'index')
    units = {}
    for name in sorted(os.listdir(idx)):
        if not name.endswith('.json'):
            continue
        with open(os.path.join(idx, name)) as fh:
            doc = json.load(fh)
        units[doc['unit']] = {f: set(v) for f, v in doc['files'].items()}
    return units


def total_lines(units):
    agg = defaultdict(set)
    for files in units.values():
        for f, lines in files.items():
            agg[f] |= lines
    return agg


def cmd_report(units):
    agg = total_lines(units)
    covered_by = defaultdict(int)
    for files in units.values():
        for f in files:
            covered_by[f] += 1

    rows = []
    for f, lines in agg.items():
        rows.append((len(lines), covered_by[f], f))
    rows.sort()

    print(f'{"cov.lines":>10} {"units":>6}  file')
    print('-' * 78)
    for n, u, f in rows[:40]:
        print(f'{n:>10} {u:>6}  {f}')
    print()
    print(f'files with ANY coverage: {len(agg)}')
    print(f'total covered lines:     {sum(len(v) for v in agg.values())}')
    print()
    print('Lowest-covered files are listed first: these are where the compiler')
    print('is least exercised, and where a new test buys the most.')


def cmd_optimize(units, target=0.995, keep_asserting=True):
    # Flatten each unit to a global line-id set so set ops are cheap.
    ids = {}
    def lid(f, ln):
        k = (f, ln)
        if k not in ids:
            ids[k] = len(ids)
        return ids[k]

    unit_sets = {}
    for u, files in units.items():
        s = set()
        for f, lines in files.items():
            for ln in lines:
                s.add(lid(f, ln))
        unit_sets[u] = s

    universe = set().union(*unit_sets.values()) if unit_sets else set()
    if not universe:
        print('optimize: no coverage data')
        return

    # --- redundancy: units adding nothing beyond the union of the others ---
    redundant = []
    for u, s in unit_sets.items():
        others = set().union(*(v for k, v in unit_sets.items() if k != u)) \
                 if len(unit_sets) > 1 else set()
        if s <= others:
            redundant.append((len(s), u))
    redundant.sort(reverse=True)

    # --- greedy set cover to `target` of the achievable universe ---
    goal = int(len(universe) * target)
    chosen, got = [], set()
    pool = dict(unit_sets)
    while len(got) < goal and pool:
        best_u, best_gain = None, -1
        for u, s in pool.items():
            gain = len(s - got)
            if gain > best_gain:
                best_u, best_gain = u, gain
        if best_gain <= 0:
            break
        chosen.append((best_u, best_gain))
        got |= pool.pop(best_u)

    dropped = [u for u in unit_sets if u not in dict(chosen)]

    print(f'units analysed:        {len(unit_sets)}')
    print(f'distinct lines covered:{len(universe)}')
    print()
    print(f'=== greedy minimal subset for {target:.1%} of achievable coverage ===')
    print(f'  keeps {len(chosen)} units, drops {len(dropped)}'
          f'  ({len(dropped)/max(1,len(unit_sets)):.0%} of the battery)')
    print(f'  coverage held: {len(got)}/{len(universe)} = {len(got)/len(universe):.2%}')
    print()
    print('  first 15 by marginal gain:')
    for u, gain in chosen[:15]:
        print(f'    +{gain:<7} {u}')
    print()
    print(f'=== strictly redundant units ({len(redundant)}) ===')
    print('  every line these touch is covered by some other unit')
    for n, u in redundant[:25]:
        print(f'    {n:>7} lines  {u}')
    if len(redundant) > 25:
        print(f'    ... and {len(redundant)-25} more')
    print()
    print('CAUTION: line coverage is not behaviour. A unit that adds no new')
    print('lines may still be the only one asserting a value, an ordering, or')
    print('an error message — the ownership pins in this repo are exactly that')
    print('shape. Treat these as review candidates, never as a delete list.')

    out = {'redundant': [u for _, u in redundant],
           'keep': [u for u, _ in chosen],
           'drop_candidates': dropped}
    with open('coverage-optimize.json', 'w') as fh:
        json.dump(out, fh, indent=1)
    print()
    print('wrote coverage-optimize.json')


def cmd_gaps(units, root):
    agg = total_lines(units)
    covered_files = set(agg)

    src_root = os.path.join(root, 'src', 'cajeta')
    all_src = []
    for dirpath, _, names in os.walk(src_root):
        for n in names:
            if n.endswith(('.cpp', '.h')):
                all_src.append(os.path.join(dirpath, n))

    uncovered = []
    for f in all_src:
        real = os.path.realpath(f)
        if not any(os.path.realpath(c) == real for c in covered_files):
            try:
                n = sum(1 for _ in open(f, errors='ignore'))
            except OSError:
                n = 0
            uncovered.append((n, f))
    uncovered.sort(reverse=True)

    print(f'=== source files with ZERO attributed coverage ({len(uncovered)}) ===')
    print('largest first — these are whole areas no test reaches\n')
    for n, f in uncovered[:40]:
        print(f'{n:>7} lines  {os.path.relpath(f, root)}')
    print()
    print(f'covered files:   {len(covered_files)}')
    print(f'uncovered files: {len(uncovered)} of {len(all_src)}')
    print()
    print('A file here is not necessarily dead — it may be reachable only from')
    print('a target this run did not exercise (a GPU backend, a platform arm).')
    print('Confirm reachability before writing a test or deleting the code.')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('cmd', choices=['report', 'optimize', 'gaps'])
    ap.add_argument('--data', required=True)
    ap.add_argument('--root', default='.')
    ap.add_argument('--target', type=float, default=0.995)
    a = ap.parse_args()

    units = load(a.data)
    if not units:
        print('no indexed units', file=sys.stderr)
        return 1
    if a.cmd == 'report':
        cmd_report(units)
    elif a.cmd == 'optimize':
        cmd_optimize(units, target=a.target)
    else:
        cmd_gaps(units, a.root)
    return 0


if __name__ == '__main__':
    sys.exit(main())
