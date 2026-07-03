#!/usr/bin/env bash
# @EntryPoint coverage check (docs-refactor plan 4.1.1).
#
# Runs `cajeta doc --emit-model-json` over the stdlib and verifies
# every class in scripts/entry-point-classes.txt has at least one
# @EntryPoint-tagged member. Prints missing classes; exit 1 on gaps.
# Also lists tagged classes absent from the audit (informational).
set -uo pipefail

REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." &>/dev/null && pwd)"
CAJETA="${CAJETA:-$REPO_ROOT/build/src/cajeta}"
AUDIT="$REPO_ROOT/scripts/entry-point-classes.txt"
STDLIB="${1:-$REPO_ROOT/runtime/src/cajeta}"

[ -x "$CAJETA" ] || { echo "compiler not found: $CAJETA" >&2; exit 2; }

MODEL="$(mktemp)"
trap 'rm -f "$MODEL"' EXIT
"$CAJETA" doc "$STDLIB" --emit-model-json 2>/dev/null > "$MODEL" \
    || { echo "cajeta doc failed over $STDLIB" >&2; exit 2; }

python3 - "$AUDIT" "$MODEL" <<'EOF'
import json, sys

audit = set()
for line in open(sys.argv[1]):
    line = line.strip()
    if line and not line.startswith('#'):
        audit.add(line)

model = json.load(open(sys.argv[2]))
tagged = set()

def members_of(t):
    yield from t.get('members', [])
    for n in t.get('nested', []):
        yield from n.get('members', [])

for pkg in model.get('packages', []):
    for t in pkg.get('types', []):
        fqn = t.get('qualifiedName') or (pkg['name'] + '.' + t['name'])
        for m in members_of(t):
            doc = m.get('doc') or {}
            for tag in doc.get('blockTags', []):
                if tag.get('name') == 'EntryPoint':
                    tagged.add(fqn)
                    break

missing = sorted(audit - tagged)
extra = sorted(tagged - audit)

for fqn in missing:
    print(f"MISSING: {fqn}")
if extra:
    print(f"(informational) tagged but not in audit: {', '.join(extra)}")
print(f"check-entrypoint-coverage: {len(audit) - len(missing)}/{len(audit)} audited classes covered")
sys.exit(1 if missing else 0)
EOF
