#!/usr/bin/env bash
# Library tour coverage (tour-quality plan 1.2.1, spec §3).
#
# Every public top-level type in the library must be reachable from the tour:
# some .cajeta source under the tour dir imports it, either by fully-qualified
# name or via a wildcard import of its package. Annotation types additionally
# count as covered when a tour source uses `@Name` — annotations resolve
# without import lines (matched by simple name, so a same-named annotation
# from another library could mask a gap; acceptable for a coverage gate).
# Nested types are covered by their outer type. Exit 1 on uncovered types,
# listing each; exit 2 on setup errors.
#
# Usage: check-library-tour-coverage.sh <library-src-root> <tour-dir> [ignore-file]
#   <library-src-root>  package-dir root passed to `cajeta doc`
#                       (e.g. src/main/cajeta)
#   <tour-dir>          tour source root, scanned recursively for imports
#   [ignore-file]       optional: one fully-qualified type name per line to
#                       exempt ('#' comments allowed)
# Env: CAJETA — compiler binary (default: build/src/cajeta beside this script,
#      for in-repo use; vendored copies must set it).
set -uo pipefail

[ $# -ge 2 ] || { echo "usage: $0 <library-src-root> <tour-dir> [ignore-file]" >&2; exit 2; }
SRC="$1"; TOUR="$2"; IGNORE="${3:-}"

SCRIPT_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." &>/dev/null && pwd)"
CAJETA="${CAJETA:-$SCRIPT_ROOT/build/src/cajeta}"

[ -x "$CAJETA" ] || { echo "compiler not found: $CAJETA (set CAJETA)" >&2; exit 2; }
[ -d "$SRC" ] || { echo "library source root not found: $SRC" >&2; exit 2; }
[ -d "$TOUR" ] || { echo "tour dir not found: $TOUR" >&2; exit 2; }
[ -z "$IGNORE" ] || [ -f "$IGNORE" ] || { echo "ignore file not found: $IGNORE" >&2; exit 2; }

MODEL="$(mktemp)"
IMPORTS="$(mktemp)"
ANNOS="$(mktemp)"
trap 'rm -f "$MODEL" "$IMPORTS" "$ANNOS"' EXIT
"$CAJETA" doc "$SRC" --emit-model-json 2>/dev/null > "$MODEL" \
    || { echo "cajeta doc failed on $SRC" >&2; exit 2; }

grep -rh '^import ' "$TOUR" --include='*.cajeta' \
    | sed 's/^import //; s/;.*//; s/[[:space:]]*$//' | sort -u > "$IMPORTS"

grep -rhoE '@[A-Za-z_][A-Za-z0-9_]*' "$TOUR" --include='*.cajeta' \
    | sed 's/^@//' | sort -u > "$ANNOS"

python3 - "$MODEL" "$IMPORTS" "$IGNORE" "$ANNOS" <<'EOF'
import json, sys

model = json.load(open(sys.argv[1]))

imported = set()      # fully-qualified type names
star_pkgs = set()     # packages covered by wildcard imports
for line in open(sys.argv[2]):
    fqn = line.strip()
    if not fqn:
        continue
    if fqn.endswith('.*'):
        star_pkgs.add(fqn[:-2])
    else:
        imported.add(fqn)

ignored = set()
if sys.argv[3]:
    for line in open(sys.argv[3]):
        entry = line.split('#', 1)[0].strip()
        if entry:
            ignored.add(entry)

used_annos = {line.strip() for line in open(sys.argv[4]) if line.strip()}

public = []
for pkg in model.get('packages', []):
    for t in pkg.get('types', []):
        if t.get('visibility') != 'public':
            continue
        fqn = t.get('qualifiedName') or f"{pkg['name']}.{t['name']}"
        if fqn in ignored:
            continue
        anno_used = t.get('kind') == 'annotation' and t['name'] in used_annos
        public.append((fqn, pkg['name'], anno_used))

missing = sorted(fqn for fqn, p, anno in public
                 if fqn not in imported and p not in star_pkgs and not anno)
for fqn in missing:
    print(f"MISSING: {fqn} (no tour source imports it)")
print(f"check-library-tour-coverage: {len(public) - len(missing)}/{len(public)} public types exercised")
sys.exit(1 if missing else 0)
EOF
