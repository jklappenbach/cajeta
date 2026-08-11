#!/usr/bin/env python3
"""Answer the three questions that shrink a test battery.

  report    per-file coverage, worst first — where is the compiler unexercised?
  overlap   pairwise overlap between units — what should be FOLDED, what is
            genuinely separate, and which units hold no unique lines at all?
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
delete list. Nothing here is a substitute for reading the test.
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


def _bitmaps(units):
    """Each unit as an int bitmask over a global line index.

    Python ints make intersection/union single machine-word-loop operations and
    .bit_count() is O(words), so the O(n^2) pass below stays tractable: 806
    suites is ~325k pairs, 5871 tests is ~17M.
    """
    ids, masks = {}, {}
    for u, files in units.items():
        m = 0
        for f, lines in files.items():
            for ln in lines:
                k = (f, ln)
                b = ids.get(k)
                if b is None:
                    b = len(ids)
                    ids[k] = b
                m |= 1 << b
        masks[u] = m
    return masks


def cmd_overlap(units, jaccard_min=0.90, top=30):
    masks = _bitmaps(units)
    names = sorted(masks)
    sizes = {u: masks[u].bit_count() for u in names}

    # --- unique lines per unit: covered by this unit and NOTHING else --------
    # The single most actionable number here. A unit with zero unique lines
    # earns its place only through its ASSERTIONS, never its reach.
    union_all = 0
    for u in names:
        union_all |= masks[u]
    unique = {}
    for u in names:
        others = 0
        for v in names:
            if v != u:
                others |= masks[v]
        unique[u] = (masks[u] & ~others).bit_count()

    subsumed, near = [], []
    for i, a in enumerate(names):
        ma, sa = masks[a], sizes[a]
        if sa == 0:
            continue
        for b in names[i + 1:]:
            mb, sb = masks[b], sizes[b]
            if sb == 0:
                continue
            inter = (ma & mb).bit_count()
            if inter == 0:
                continue
            union = sa + sb - inter
            j = inter / union if union else 0.0
            if inter == sa and inter == sb:
                near.append((1.0, a, b, inter, 'IDENTICAL'))
            elif inter == sa:
                subsumed.append((sa, a, b))          # a's lines ⊆ b's
            elif inter == sb:
                subsumed.append((sb, b, a))          # b's lines ⊆ a's
            elif j >= jaccard_min:
                near.append((j, a, b, inter, 'NEAR'))

    near.sort(reverse=True)
    subsumed.sort(reverse=True)
    zero_unique = sorted((sizes[u], u) for u in names if unique[u] == 0 and sizes[u])

    print(f'units: {len(names)}   distinct lines: {union_all.bit_count()}')
    print()
    print(f'=== units holding NO unique lines ({len(zero_unique)}) ===')
    print('  every line is reachable from some other unit, so these earn their')
    print('  place through assertions alone — review, do not auto-delete')
    for n, u in zero_unique[:top]:
        print(f'    {n:>7} lines  {u}')
    if len(zero_unique) > top:
        print(f'    ... and {len(zero_unique)-top} more')
    print()
    print(f'=== subsumed: A covers a strict SUBSET of B ({len(subsumed)}) ===')
    print('  strongest fold signal — A walks no code B does not already walk')
    for n, a, b in subsumed[:top]:
        print(f'    {n:>7} lines  {a}')
        print(f'    {"":>7}         `-- inside --> {b}')
    if len(subsumed) > top:
        print(f'    ... and {len(subsumed)-top} more')
    print()
    print(f'=== near-duplicate pairs (Jaccard >= {jaccard_min:.2f}) ({len(near)}) ===')
    for j, a, b, inter, kind in near[:top]:
        print(f'    {j:.3f} {kind:<9} {inter:>6} shared')
        print(f'          {a}')
        print(f'          {b}')
    if len(near) > top:
        print(f'    ... and {len(near)-top} more')

    # --- fold clusters: connected components over the near-duplicate graph ---
    parent = {u: u for u in names}
    def find(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x
    for _, a, b, _, _ in near:
        ra, rb = find(a), find(b)
        if ra != rb:
            parent[ra] = rb
    groups = {}
    for u in names:
        groups.setdefault(find(u), []).append(u)
    clusters = [g for g in groups.values() if len(g) > 1]
    clusters.sort(key=len, reverse=True)

    print()
    print(f'=== fold clusters ({len(clusters)}) ===')
    print('  mutually near-duplicate units — candidates to merge into ONE')
    print('  program, which keeps every assertion but pays ONE process start')
    for g in clusters[:12]:
        print(f'    [{len(g)}] ' + ', '.join(sorted(g)[:6])
              + (' ...' if len(g) > 6 else ''))

    out = {'zero_unique': [u for _, u in zero_unique],
           'subsumed': [{'inner': a, 'outer': b} for _, a, b in subsumed],
           'near_duplicates': [{'jaccard': round(j, 4), 'a': a, 'b': b}
                               for j, a, b, _, _ in near],
           'fold_clusters': clusters,
           'unique_lines': unique}
    with open('coverage-overlap.json', 'w') as fh:
        json.dump(out, fh, indent=1)
    print()
    print('wrote coverage-overlap.json')
    print()
    print('FOLD is not DELETE. Two units with identical coverage can assert')
    print('different things — the lend/transfer pins in this repo walk the same')
    print('ArrayList lines while checking opposite title outcomes. Merging them')
    print('into one program keeps both assertions and removes one process')
    print('startup, which is where this battery actually spends its time.')


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
    # Count-based, O(total lines): a unit is redundant iff none of its lines
    # has coverage-count 1. (The naive per-unit union-of-others is O(n^2) in
    # set inserts and ran for DAYS at ~5,900 units.)
    from collections import Counter
    line_count = Counter()
    for s in unit_sets.values():
        line_count.update(s)
    redundant = [(len(s), u) for u, s in unit_sets.items()
                 if s and not any(line_count[i] == 1 for i in s)]
    redundant.sort(reverse=True)

    # --- greedy set cover to `target` of the achievable universe ---
    # Big-int bitmaps: AND/OR/bit_count run at C speed.
    masks = {}
    for u, s in unit_sets.items():
        m = 0
        for i in s:
            m |= 1 << i
        masks[u] = m
    goal = int(len(universe) * target)
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
        chosen.append((best_u, best_gain))
        got |= pool.pop(best_u)
    got = {i for i in universe if (got >> i) & 1}

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


def cmd_percent(units, totals_path, extra_dirs, exclude):
    """test-battery-restructure 3.2 — covered/total/percent per file + overall.

    The denominator is totals.json (index_gcov.py --totals): every executable
    line gcov attributes to src/cajeta, including files no test touches. The
    numerator is the union of covered lines across the given index dirs.
    --exclude drops device-gated files from BOTH sides (spec 4.4: they count
    only on runners with the device)."""
    with open(totals_path) as fh:
        totals = {f: set(v) for f, v in json.load(fh)['files'].items()}
    covered = total_lines(units)
    for d in extra_dirs:
        for f, lines in total_lines(load(d)).items():
            covered.setdefault(f, set()).update(lines)
    pats = [re.compile(p) for p in exclude]
    rows = []
    tot_num = tot_den = 0
    for f, ex_lines in sorted(totals.items()):
        short = f.split('/src/cajeta/')[-1]
        if any(p.search(short) for p in pats):
            continue
        # Clamp the numerator to the denominator's line set: a covered line
        # not in the gcno table (compiler version skew) must not inflate %.
        cov = len(covered.get(f, set()) & ex_lines)
        rows.append((cov / len(ex_lines) if ex_lines else 1.0,
                     cov, len(ex_lines), short))
        tot_num += cov
        tot_den += len(ex_lines)
    rows.sort()
    print(f'{"pct":>6}  {"cov":>6}  {"total":>6}  file')
    for pct, cov, tot, short in rows:
        print(f'{pct*100:5.1f}%  {cov:6d}  {tot:6d}  {short}')
    print()
    n90 = sum(1 for r in rows if r[0] >= 0.90)
    print(f'files at >=90%: {n90}/{len(rows)}')
    print(f'OVERALL: {tot_num}/{tot_den} = {tot_num/tot_den*100:.2f}%'
          f'  (target: 90% of executable src/cajeta lines, spec 4.5)')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('cmd', choices=['report', 'overlap', 'optimize', 'gaps',
                                    'percent'])
    ap.add_argument('--data', required=True)
    ap.add_argument('--root', default='.')
    ap.add_argument('--target', type=float, default=0.995)
    ap.add_argument('--jaccard', type=float, default=0.90)
    ap.add_argument('--top', type=int, default=30)
    ap.add_argument('--totals', default=None,
                    help='percent: totals.json from index_gcov.py --totals '
                         '(default <data>/totals.json)')
    ap.add_argument('--also', action='append', default=[],
                    help='percent: additional index data dirs to union into '
                         'the numerator (e.g. a refold measurement)')
    ap.add_argument('--exclude', action='append', default=[],
                    help='percent: regex of files excluded from the metric '
                         '(device-gated code, spec 4.4)')
    a = ap.parse_args()

    units = load(a.data)
    if not units:
        print('no indexed units', file=sys.stderr)
        return 1
    if a.cmd == 'report':
        cmd_report(units)
    elif a.cmd == 'overlap':
        cmd_overlap(units, jaccard_min=a.jaccard, top=a.top)
    elif a.cmd == 'optimize':
        cmd_optimize(units, target=a.target)
    elif a.cmd == 'percent':
        cmd_percent(units, a.totals or os.path.join(a.data, 'totals.json'),
                    a.also, a.exclude)
    else:
        cmd_gaps(units, a.root)
    return 0


if __name__ == '__main__':
    sys.exit(main())
