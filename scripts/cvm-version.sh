#!/usr/bin/env bash
# Print cvm's declared version, from tools/cvm/cajeta.json.
#
# cvm's version is its OWN, not the compiler's: a 1.0 cvm installs an 8.0
# cajeta, so the release tag cannot speak for it. The manifest is the source of
# truth and `Cvm.cajeta`'s VERSION constant has to agree, because that is what
# `cvm --version` prints and a package manager reporting one number while the
# binary reports another is a bug report nobody can place.
# scripts/check-cvm-version.sh asserts they agree.
#
# cajeta.json is JSONC, so `//` comments are stripped before parsing.
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
MANIFEST="${1:-${ROOT}/tools/cvm/cajeta.json}"

[ -f "$MANIFEST" ] || { echo "error: no manifest at $MANIFEST" >&2; exit 1; }

MANIFEST="$MANIFEST" python3 - <<'PY'
import json, os, re, sys

text = open(os.environ["MANIFEST"], encoding="utf-8").read()
# Strip // line comments that are not inside a string. Crude but sufficient for
# this manifest, and wrong quietly is worse than wrong loudly, so the result is
# validated by json.loads immediately after.
out, in_str, esc, i = [], False, False, 0
while i < len(text):
    c = text[i]
    if in_str:
        out.append(c)
        if esc:
            esc = False
        elif c == "\\":
            esc = True
        elif c == '"':
            in_str = False
        i += 1
        continue
    if c == '"':
        in_str = True
        out.append(c)
        i += 1
        continue
    if c == "/" and i + 1 < len(text) and text[i + 1] == "/":
        while i < len(text) and text[i] != "\n":
            i += 1
        continue
    out.append(c)
    i += 1

doc = json.loads("".join(out))
v = doc.get("details", {}).get("version")
if not v:
    sys.exit("error: tools/cvm/cajeta.json has no details.version")
print(v)
PY
