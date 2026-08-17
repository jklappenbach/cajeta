#!/usr/bin/env python3
"""
report_return_titles.py — turn a harvest of `[return-title]` notes into the
Unit 8 ride-through inventory.

stdlib-ownership-convention plan, Unit 8 (8.1.1). Unit 8 asks whether the
RETURN still carries the ownership decision; that turns on how many plain-return
methods hand out a title anyway. The measurement is the compiler's own:
build anything with

    CAJETA_AUDIT_RETURN_TITLES=1 cajeta build 2> harvest.log

and every plain (non-`#`) class-pointer return codegen reaches emits

    cajeta: note: [return-plain] <class>.<method> returns=<T>          # denominator
    cajeta: note: [return-title] <class>.<method>:<line> carry=… via=… returns=<T>

This script unions those across logs (a method compiles once per module that
pulls it in, and once per library build) and writes the inventory.

Deliberately NOT a source-shape audit: 3.3.3 recorded what that costs — the
static pass keys on shapes it recognises, the compiler sees every one. The
coverage limit here is the honest one and it is stated in the output: a method
no build compiled is a method nobody measured.

Usage: report_return_titles.py <harvest.log> [more.log ...] > inventory.md
"""

import re
import sys
from collections import defaultdict

PLAIN_RE = re.compile(
    r'\[return-plain\] (?P<key>[\w.$<>]+) returns=(?P<ret>\S+)')
TITLE_RE = re.compile(
    r'\[return-title\] (?P<key>[\w.$<>]+):(?P<line>\d+) '
    r'carry=(?P<carry>\w+) via=(?P<via>[\w-]+) returns=(?P<ret>\S+)'
    r'(?: callee=(?P<callee>\S+) callee-owns=(?P<owns>[01]))?')

# What each mechanism means for the developer reading the signature, so the
# inventory carries the interpretation next to the count rather than in a
# separate document that drifts from it.
VIA_MEANING = {
    'call-ride#': 'tail-calls a method that DECLARES `#`; a title rides out '
                  'through a signature that says borrow — the `viaPlain` shape',
    'call-ride': 'tail-calls a plain-return method; the decision is deferred '
                 'one more frame (the fluent-builder chain)',
    'call-ride?': 'calls a CLOSURE (`Stream.fold<R>`); the callback is a '
                  'parameter, so nothing static decides at all',
    'formal': 'returns a parameter; the caller gets back whatever mode it lent '
              '— genuinely runtime-variable',
    'move': '`return #x`; forwards the mode the frame holds (not an assertion '
            'of title — CLAUDE.md §2.2)',
    'mode-carry': '`return #= x`; releases whatever title the frame holds, by '
                  'design (the collection remove shape)',
    'flagged': '`return Cajeta.flagged(v, owned)`; the container decides',
    'other': 'a runtime flag from a shape the classifier does not name yet — '
             'inspect before counting',
}


def strip_generics(key):
    """`cajeta.lang.Optional<cajeta.io.file.Path>.get` -> `cajeta.lang.Optional.get`.

    The unit of the enumeration is the SOURCE method a developer would have to
    change, not the instantiation. `Optional.get` compiled against eleven
    element types is one signature with one ownership stance; counting it
    eleven times would inflate the ride-through number by exactly the amount
    that makes the generic collections look like the problem.
    """
    out = []
    depth = 0
    for ch in key:
        if ch == '<':
            depth += 1
        elif ch == '>':
            depth = max(0, depth - 1)
        elif depth == 0:
            out.append(ch)
    return ''.join(out)


def parse(paths):
    considered = {}                      # class.method -> return type
    carriers = defaultdict(list)         # class.method -> [record]
    for path in paths:
        with open(path, encoding='utf-8', errors='replace') as fh:
            for line in fh:
                m = PLAIN_RE.search(line)
                if m:
                    considered.setdefault(strip_generics(m.group('key')),
                                          m.group('ret'))
                    continue
                m = TITLE_RE.search(line)
                if m:
                    key = strip_generics(m.group('key'))
                    considered.setdefault(key, m.group('ret'))
                    via = m.group('via')
                    # A ride splits three ways by what it rides — see
                    # VIA_MEANING. Doing it here keeps one vocabulary for the
                    # buckets, the table, and the counts.
                    if via == 'call-ride':
                        if m.group('callee') == '(closure)':
                            via = 'call-ride?'
                        elif m.group('owns') == '1':
                            via = 'call-ride#'
                    rec = (int(m.group('line')), m.group('carry'),
                           via, m.group('ret'))
                    if rec not in carriers[key]:
                        carriers[key].append(rec)
    return considered, carriers


def package_of(key):
    """`cajeta.codec.json.JsonObject.keyAt` -> `cajeta.codec.json`."""
    parts = key.split('.')
    return '.'.join(parts[:-2]) if len(parts) > 2 else '(default)'


def coverage(paths):
    """Per-log: what it measured, and whether the build ran to the end.

    A build that stops early measured everything up to that point and nothing
    after, so the denominator is a FLOOR. Stating which logs are truncated (and
    on what) is the difference between a coverage limit and a silent one.
    """
    rows = []
    for path in paths:
        plain = titles = 0
        stopped = ''
        with open(path, encoding='utf-8', errors='replace') as fh:
            for line in fh:
                if '[return-plain]' in line:
                    plain += 1
                elif '[return-title]' in line:
                    titles += 1
                elif 'CAJETA_ERROR_' in line and not stopped:
                    err = line.split('CAJETA_ERROR_', 1)[1].split(':', 1)[0]
                    stopped = 'CAJETA_ERROR_' + err
        rows.append((path.rsplit('/', 1)[-1], plain, titles, stopped))
    return rows


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    considered, carriers = parse(sys.argv[1:])
    cov = coverage(sys.argv[1:])

    n_seen = len(considered)
    n_carry = len(carriers)
    pct = (100.0 * n_carry / n_seen) if n_seen else 0.0

    by_via = defaultdict(set)
    for key, recs in carriers.items():
        for _line, _carry, via, _ret in recs:
            by_via[via].add(key)

    print('# Return-side title audit — the ride-through enumeration')
    print()
    print('Generated by `tools/ownership/report_return_titles.py` from a build')
    print('run with `CAJETA_AUDIT_RETURN_TITLES=1`. Plan item 8.1.1; see')
    print('`specs/stdlib-ownership-convention-spec.md`.')
    print()
    print('The numbers are the COMPILER\'s decision at each return')
    print('(`ReturnStatement::generateCode`\'s `returnTitleFlag`), not a')
    print('source-shape classification. Coverage is what the harvested builds')
    print('compiled: a method no build reached is a method nobody measured.')
    print()
    print('| Measure | Count |')
    print('|---------|-------|')
    print('| Plain-return, class-returning methods compiled | %d |' % n_seen)
    print('| …of those, methods whose return can carry a title | %d |' % n_carry)
    print('| Ride-through rate | %.1f%% |' % pct)
    print()
    print('## By mechanism')
    print()
    print('| Mechanism | Methods | What the caller sees |')
    print('|-----------|---------|----------------------|')
    for via in sorted(by_via, key=lambda v: -len(by_via[v])):
        print('| `%s` | %d | %s |'
              % (via, len(by_via[via]), VIA_MEANING.get(via, '—')))
    print()

    print('## Coverage')
    print()
    # 8.1.3 — the floor/census distinction is a FACT of this run, not
    # boilerplate: when every harvested build ran to completion the number IS
    # the census the item asked for, and saying "floor" would under-claim it.
    all_complete = all(not stopped for _n, _p, _t, stopped in cov)
    print('One row per harvested build.', end=' ')
    if all_complete:
        print('Every build ran to completion, so the')
        print('denominator above is a CENSUS of what these libraries compile —')
        print('the only remaining coverage limit is stdlib methods no library')
        print('reaches.')
    else:
        print('A truncated build measured everything')
        print('it compiled before it stopped and nothing after, so the denominator')
        print('above is a FLOOR, not a census.')
    print()
    print('| Harvest | Plain returns seen | Title notes | Ran to completion |')
    print('|---------|--------------------|-------------|-------------------|')
    for name, plain, titles, stopped in cov:
        end = 'yes' if not stopped else 'no — stopped at `%s`' % stopped
        print('| `%s` | %d | %d | %s |' % (name, plain, titles, end))
    print()

    by_pkg = defaultdict(list)
    for key in carriers:
        by_pkg[package_of(key)].append(key)
    print('## By package')
    print()
    print('| Package | Ride-through methods |')
    print('|---------|----------------------|')
    for pkg in sorted(by_pkg):
        print('| `%s` | %d |' % (pkg, len(by_pkg[pkg])))
    print()

    print('## Every site')
    print()
    print('One row per `return`. A generic method compiled against several')
    print('element types is ONE row — the instantiations agree on the stance,')
    print('and the extra return type is noted rather than repeated.')
    print()
    print('| Method | Line | Carry | Via | Returns |')
    print('|--------|------|-------|-----|---------|')
    for key in sorted(carriers):
        sites = defaultdict(set)          # (line, carry, via) -> {return type}
        for line, carry, via, ret in carriers[key]:
            sites[(line, carry, via)].add(ret)
        for (line, carry, via), rets in sorted(sites.items()):
            shown = sorted(rets)[0]
            extra = (' (+%d instantiations)' % (len(rets) - 1)
                     if len(rets) > 1 else '')
            print('| `%s` | %d | %s | `%s` | `%s`%s |'
                  % (key, line, carry, via, shown, extra))
    print()


if __name__ == '__main__':
    main()
