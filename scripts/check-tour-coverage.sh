#!/usr/bin/env bash
# Tour package coverage (docs-refactor plan 12.3.1).
#
# Every stdlib package with @EntryPoint-tagged methods must be exercised
# by the tour: some .cajeta source under samples/tour/ (main tour or a
# separate entry point like the xpu tour) imports a class from it.
# Registration of main-tour demos is check-tour.sh's job; this script
# only checks package reach. Exit 1 on uncovered packages.
set -uo pipefail

REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." &>/dev/null && pwd)"
CAJETA="${CAJETA:-$REPO_ROOT/build/src/cajeta}"
STDLIB="$REPO_ROOT/runtime/src/cajeta"
TOUR="$REPO_ROOT/samples/tour"

[ -x "$CAJETA" ] || { echo "compiler not found: $CAJETA" >&2; exit 2; }

MODEL="$(mktemp)"
IMPORTS="$(mktemp)"
trap 'rm -f "$MODEL" "$IMPORTS"' EXIT
"$CAJETA" doc "$STDLIB" --emit-model-json 2>/dev/null > "$MODEL" \
    || { echo "cajeta doc failed" >&2; exit 2; }

grep -rh '^import cajeta\.' "$TOUR" --include='*.cajeta' \
    | sed 's/^import //; s/;.*//' | sort -u > "$IMPORTS"

python3 - "$MODEL" "$IMPORTS" <<'EOF'
import json, sys

model = json.load(open(sys.argv[1]))

def members_of(t):
    yield from t.get('members', [])
    for n in t.get('nested', []):
        yield from n.get('members', [])

tagged_pkgs = set()
for pkg in model.get('packages', []):
    for t in pkg.get('types', []):
        for m in members_of(t):
            doc = m.get('doc') or {}
            if any(tag.get('name') == 'EntryPoint' for tag in doc.get('blockTags', [])):
                tagged_pkgs.add(pkg['name'])
                break

covered = set()
for line in open(sys.argv[2]):
    fqn = line.strip()          # cajeta.a.b.Class
    covered.add(fqn.rsplit('.', 1)[0])

missing = sorted(tagged_pkgs - covered)
for p in missing:
    print(f"MISSING: {p} (no tour source imports it)")
print(f"check-tour-coverage: {len(tagged_pkgs) - len(missing)}/{len(tagged_pkgs)} tagged packages exercised")
sys.exit(1 if missing else 0)
EOF
