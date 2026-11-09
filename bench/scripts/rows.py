#!/usr/bin/env python3
"""Row bookkeeping for bench.sh over a run's results.csv (benchmark-reference 22.2.9).

  rows.py packages <results.csv>        the catalog packages that hold competitor
                                        rows (language != cajeta), one per line
  rows.py strip-partial <results.csv>   drop every row whose field count differs
                                        from the header's (a cut pass's last row),
                                        rewrite in place, print the count dropped
  rows.py partial <results.csv>         the count strip-partial would drop, no rewrite
  rows.py count <results.csv>           the number of data rows
"""
import csv
import os
import sys


def read(path):
    with open(path, newline="", encoding="utf-8") as f:
        return list(csv.reader(f))


def packages(path):
    table = read(path)
    if not table:
        return []
    header, body = table[0], table[1:]
    area, lang = header.index("area"), header.index("language")
    seen = []
    for r in body:
        if len(r) != len(header) or r[lang] == "cajeta":
            continue
        pkg = r[area].split("/")[0]
        if pkg not in seen:
            seen.append(pkg)
    return seen


def strip_partial(path, rewrite=True):
    table = read(path)
    if not table:
        return 0
    header, body = table[0], table[1:]
    kept = [r for r in body if len(r) == len(header)]
    dropped = len(body) - len(kept)
    if dropped and rewrite:
        tmp = path + ".tmp"
        with open(tmp, "w", newline="", encoding="utf-8") as f:
            w = csv.writer(f, lineterminator="\n")
            w.writerow(header)
            w.writerows(kept)
        os.replace(tmp, path)
    return dropped


def main(argv):
    if len(argv) != 2 or argv[0] not in ("packages", "strip-partial", "partial", "count"):
        print(__doc__.strip(), file=sys.stderr)
        return 2
    cmd, path = argv
    if cmd == "packages":
        for p in packages(path):
            print(p)
    elif cmd == "strip-partial":
        print(strip_partial(path))
    elif cmd == "partial":
        print(strip_partial(path, rewrite=False))
    else:
        print(max(len(read(path)) - 1, 0))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
