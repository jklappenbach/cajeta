#!/usr/bin/env python3
"""
audit_ownership.py — inventory the stdlib's ownership stances.

stdlib-ownership-convention plan, Unit 1 (1.2.1). Re-run after the
Unit 4 migration as a regression: the findings that remain must all be
on the documented exception list.

What it looks for, per spec section:

  VIEW-RETURN (2.2)   a public method returning a plain (non-`#`) class,
                      array, or String type whose body returns interior
                      state — `return this.f` / `return this.f[i]`.
                      Correct as a view; the hazard is a caller writing
                      `#` on the result (the JsonObject.keyAt bug).

  PRODUCER? (2.1)     a public method whose NAME reads as a producer
                      (`to...`, `as...`, `read...`, `encode...`,
                      `copy...`) but which returns a plain type. These
                      are the ones most likely to be mistaken for owned.

  CAPTURE (2.4)       a plain (non-`#`) parameter stored beyond the call:
                      `this.f = p` (plain store — a captured borrow) or
                      `this.f #= p` (caller's-choice; legitimate ONLY for
                      sinks, spec 2.3).

  CONDITIONAL (2.6)   a method whose body branches on a property of an
                      argument and takes a DIFFERENT ownership path per
                      branch. This is the `setString(String)` shape and
                      the one the convention flatly bans.

Output is a markdown table on stdout. Classification is deliberately
NOT automated beyond flagging: spec 2.3 (sinks) cannot be decided from
syntax, and a script that guessed would launder judgement into data.
"""

import os
import re
import sys
from collections import defaultdict

METHOD_RE = re.compile(
    r'^[ \t]*(?:(public|protected|private)\s+)?'
    r'(?:(static)\s+)?(?:(final)\s+)?'
    r'(#?)([A-Za-z_][\w.]*(?:<[^;{()]*>)?(?:\[\])*)\s+'
    r'(\w+)\s*\(([^)]*)\)\s*(\{|;)')

FIELD_RE = re.compile(
    r'^[ \t]{4}(?:(?:public|protected|private)\s+)?'
    r'(?:static\s+)?(?:final\s+)?'
    r'([A-Za-z_][\w.]*(?:<[^;={]*>)?(?:\[\])*)\s+(\w+)\s*[;=]')

PRIMITIVES = {
    'void', 'boolean', 'int8', 'int16', 'int32', 'int64', 'int128',
    'uint8', 'uint16', 'uint32', 'uint64', 'float16', 'float32',
    'float64', 'float128', 'bfloat16', 'char',
}

PRODUCER_PREFIXES = ('to', 'as', 'read', 'encode', 'decode', 'copy',
                     'clone', 'format', 'build', 'make', 'create')


def is_primitive(t):
    base = t.replace('[]', '')
    return base in PRIMITIVES


def method_body(lines, start):
    """Return (body_text, end_index) for a method whose `{` is on `start`."""
    depth = 0
    out = []
    for i in range(start, len(lines)):
        line = lines[i]
        out.append(line)
        depth += line.count('{') - line.count('}')
        if depth <= 0 and i > start:
            return '\n'.join(out), i
        if depth <= 0 and '{' in line and '}' in line:
            return '\n'.join(out), i
    return '\n'.join(out), len(lines) - 1


def audit(path, rel):
    lines = open(path, encoding='utf-8').read().split('\n')
    fields = {}
    for ln in lines:
        m = FIELD_RE.match(ln)
        if m and not re.match(r'^\s*(return|if|while|throw)\b', ln):
            fields[m.group(2)] = m.group(1)

    findings = []
    i = 0
    while i < len(lines):
        m = METHOD_RE.match(lines[i])
        if not m:
            i += 1
            continue
        vis, static, _final, owned, rtype, name, params, tail = m.groups()
        if name in ('if', 'while', 'for', 'switch', 'catch', 'return'):
            i += 1
            continue
        if tail == ';':                       # @Native / abstract
            i += 1
            continue
        body, end = method_body(lines, i)
        public = (vis == 'public')

        # --- VIEW-RETURN / PRODUCER? -------------------------------
        if public and not owned and not is_primitive(rtype) \
                and rtype != 'void':
            returns_interior = False
            for f in fields:
                if re.search(r'return\s+this\.' + f + r'\s*[;\[]', body) \
                        or re.search(r'return\s+this\.' + f + r'\[[^\]]*\]\s*;',
                                     body):
                    returns_interior = True
                    break
            if returns_interior:
                kind = 'VIEW-RETURN'
                if name.startswith(PRODUCER_PREFIXES):
                    kind = 'PRODUCER?'
                findings.append((kind, rel, name, rtype, ''))

        # --- CAPTURE ------------------------------------------------
        for p in [p.strip() for p in params.split(',') if p.strip()]:
            pm = re.match(r'(#?)([A-Za-z_][\w.]*(?:<[^>]*>)?(?:\[\])*)\s+(\w+)$',
                          p)
            if not pm:
                continue
            p_owned, p_type, p_name = pm.groups()
            if p_owned or is_primitive(p_type):
                continue
            if re.search(r'this\.\w+\s*#=\s*' + p_name + r'\s*;', body):
                findings.append(('CAPTURE(#=)', rel, name, p_type,
                                 p_name + ' — sink model; legitimate only '
                                 'if this type IS a sink (2.3)'))
            elif re.search(r'this\.\w+\s*=\s*' + p_name + r'\s*;', body):
                findings.append(('CAPTURE(=)', rel, name, p_type,
                                 p_name + ' — plain store of a borrow'))
            elif re.search(r'this\.\w+\[[^\]]*\]\s*#?=\s*' + p_name + r'\s*;',
                           body):
                findings.append(('CAPTURE(elem)', rel, name, p_type,
                                 p_name + ' — stored into an element'))

        # --- CONDITIONAL --------------------------------------------
        if 'setStringOwned' in body and 'setString(' in body:
            findings.append(('CONDITIONAL', rel, name, rtype,
                             'branches between owned and borrowed paths'))
        elif body.count('#=') and re.search(r'\bif\s*\(', body) \
                and re.search(r'return\s+this\.set\w+Owned', body):
            findings.append(('CONDITIONAL', rel, name, rtype,
                             'ownership differs per branch'))
        i = end + 1
    return findings


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else 'runtime/src/cajeta'
    all_findings = []
    for dirpath, _dirs, files in os.walk(root):
        for f in sorted(files):
            if not f.endswith('.cajeta'):
                continue
            p = os.path.join(dirpath, f)
            rel = os.path.relpath(p, root)
            all_findings.extend(audit(p, rel))

    by_kind = defaultdict(list)
    for k, rel, name, t, note in all_findings:
        by_kind[k].append((rel, name, t, note))

    print('# stdlib ownership audit')
    print()
    print('Generated by `tools/ownership/audit_ownership.py`; see')
    print('`specs/stdlib-ownership-convention-spec.md`.')
    print()
    print('| Kind | Count |')
    print('|------|-------|')
    for k in sorted(by_kind):
        print('| %s | %d |' % (k, len(by_kind[k])))
    print()
    for k in sorted(by_kind):
        print('## %s' % k)
        print()
        print('| File | Method | Type | Note |')
        print('|------|--------|------|------|')
        for rel, name, t, note in sorted(by_kind[k]):
            print('| `%s` | `%s` | `%s` | %s |' % (rel, name, t, note))
        print()


if __name__ == '__main__':
    main()
