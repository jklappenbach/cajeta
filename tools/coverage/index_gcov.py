#!/usr/bin/env python3
"""Turn per-unit .gcda trees into per-unit covered-line sets.

One JSON per unit: {"unit": name, "files": {src_path: [covered line numbers]}}.
That is the input every downstream question needs — redundancy, minimal subset,
and gaps are all set operations over these.

`gcov --json-format` is used directly so neither lcov nor gcovr is required
(neither is installed here, and gcovr would only re-derive what gcov emits).
"""
import argparse, gzip, json, os, subprocess, sys, tempfile
from concurrent.futures import ProcessPoolExecutor


def index_unit(args):
    unit_dir, build_dir = args
    unit = os.path.basename(unit_dir)
    gcda = []
    for dirpath, _, names in os.walk(unit_dir):
        gcda += [os.path.join(dirpath, n) for n in names if n.endswith('.gcda')]
    if not gcda:
        # A unit that produced no .gcda ran but covered nothing attributable —
        # usually a crash before exit. Record it as empty rather than dropping
        # it, so `optimize` can still see the unit exists and cost time.
        return unit, {}, len(gcda)

    files = {}
    with tempfile.TemporaryDirectory() as tmp:
        # gcov (GCC 15) resolves BOTH the .gcno and the .gcda from one
        # directory: with `-o objdir` it recomposes the DATA path under objdir
        # too and ignores the positional gcda ("cannot open data file,
        # assuming not executed"). So instead of -o, symlink each object's
        # .gcno from the build tree NEXT TO the unit's .gcda and let gcov use
        # the positional path's directory for both.
        for g in gcda:
            rel = os.path.relpath(g, unit_dir)
            obj_dir = os.path.join(build_dir, os.path.dirname(rel))
            gcno_src = os.path.abspath(os.path.join(
                build_dir, rel[:-len('.gcda')] + '.gcno'))
            gcno_dst = g[:-len('.gcda')] + '.gcno'
            if not os.path.isdir(obj_dir):
                continue
            if os.path.exists(gcno_src) and not os.path.exists(gcno_dst):
                try:
                    os.symlink(gcno_src, gcno_dst)
                except OSError:
                    continue
            try:
                p = subprocess.run(
                    ['gcov', '--json-format', '--stdout', os.path.abspath(g)],
                    cwd=tmp, capture_output=True, timeout=120, check=False)
                raw = p.stdout
                if raw[:2] == b'\x1f\x8b':
                    raw = gzip.decompress(raw)
                if not raw.strip():
                    continue
                for line in raw.splitlines():
                    line = line.strip()
                    if not line.startswith(b'{'):
                        continue
                    doc = json.loads(line)
                    for f in doc.get('files', []):
                        src = f.get('file', '')
                        # Only OUR sources; skip system headers and generated
                        # ANTLR front end, which nobody is going to hand-test.
                        if '/src/cajeta/' not in src:
                            continue
                        hit = {ln['line_number'] for ln in f.get('lines', [])
                               if ln.get('count', 0) > 0}
                        if hit:
                            files.setdefault(src, set()).update(hit)
            except Exception:
                continue
    return unit, {k: sorted(v) for k, v in files.items()}, len(gcda)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--build', required=True)
    ap.add_argument('--data', required=True)
    ap.add_argument('--jobs', type=int, default=8)
    a = ap.parse_args()

    units_root = os.path.join(a.data, 'units')
    unit_dirs = [os.path.join(units_root, d) for d in sorted(os.listdir(units_root))
                 if os.path.isdir(os.path.join(units_root, d))]
    if not unit_dirs:
        print('index: no unit directories', file=sys.stderr)
        return 1

    out_dir = os.path.join(a.data, 'index')
    os.makedirs(out_dir, exist_ok=True)
    done = 0
    with ProcessPoolExecutor(max_workers=a.jobs) as ex:
        for unit, files, n in ex.map(index_unit,
                                     [(d, a.build) for d in unit_dirs]):
            with open(os.path.join(out_dir, unit + '.json'), 'w') as fh:
                json.dump({'unit': unit, 'files': files}, fh)
            done += 1
            if done % 25 == 0:
                print(f'  indexed {done}/{len(unit_dirs)}', file=sys.stderr)
    print(f'>> indexed {done} units -> {out_dir}', file=sys.stderr)
    return 0


if __name__ == '__main__':
    sys.exit(main())
