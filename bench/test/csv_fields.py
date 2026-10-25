#!/usr/bin/env python3
"""csv_fields.py [file ...] — a results.csv (or competitor rows) split by the
csv module, one record per line, fields joined by the ASCII unit separator
(0x1F), for the smokes' awk and `read` checks (test/csv_lib.sh).

Since dev.cajeta.bench 0.1.3 free-text fields are RFC 4180-quoted (a comma, a
quote or a newline in notes, flags, os, driver, model, platform), so
`awk -F,` / `cut -d,` / `IFS=,` read the wrong column. This reads the CSV
properly (the same parse as the library's tools/csv-column.py) and writes
fields that need no quoting: a newline inside a field becomes a space.

Input already in this form (it holds a 0x1F) passes through unchanged, so a
row printed by one check can be fed to the next. With no file, reads stdin.
"""
import csv
import io
import sys

US = "\x1f"


def emit(text, out):
    if US in text:
        out.write(text if text.endswith("\n") or not text else text + "\n")
        return
    for rec in csv.reader(io.StringIO(text, newline="")):
        out.write(US.join(f.replace("\r", " ").replace("\n", " ") for f in rec) + "\n")


def main(argv):
    out = sys.stdout
    if not argv or argv == ["-"]:
        emit(sys.stdin.read(), out)
        return 0
    for path in argv:
        if path == "-":
            emit(sys.stdin.read(), out)
            continue
        with open(path, newline="", encoding="utf-8", errors="replace") as f:
            emit(f.read(), out)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
