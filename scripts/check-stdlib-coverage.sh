#!/usr/bin/env bash
# Stdlib reference coverage check (docs-refactor plan 10.1.1).
#
# Runs `cajeta doc --emit-model-json` over the stdlib and verifies every
# class with an @EntryPoint-tagged member has a reference document at
# docs/stdlib/<package minus cajeta.>/<Class>.md (spec 3.1.1).
# Prints missing documents; exit 1 on gaps. Also lists reference documents
# that map to no tagged class (informational — stale or hand-added).
set -uo pipefail

REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." &>/dev/null && pwd)"
CAJETA="${CAJETA:-$REPO_ROOT/build/src/cajeta}"
STDLIB="${1:-$REPO_ROOT/runtime/src/cajeta}"
DOCS="$REPO_ROOT/docs/stdlib"

[ -x "$CAJETA" ] || { echo "compiler not found: $CAJETA" >&2; exit 2; }

MODEL="$(mktemp)"
trap 'rm -f "$MODEL"' EXIT
"$CAJETA" doc "$STDLIB" --emit-model-json 2>/dev/null > "$MODEL" \
    || { echo "cajeta doc failed over $STDLIB" >&2; exit 2; }

python3 - "$MODEL" "$DOCS" <<'EOF'
import json, os, sys

model = json.load(open(sys.argv[1]))
docs_root = sys.argv[2]

def members_of(t):
    yield from t.get('members', [])
    for n in t.get('nested', []):
        yield from n.get('members', [])

tagged = set()
for pkg in model.get('packages', []):
    for t in pkg.get('types', []):
        fqn = t.get('qualifiedName') or (pkg['name'] + '.' + t['name'])
        for m in members_of(t):
            doc = m.get('doc') or {}
            if any(tag.get('name') == 'EntryPoint' for tag in doc.get('blockTags', [])):
                tagged.add(fqn)
                break

def doc_path(fqn):
    parts = fqn.split('.')
    assert parts[0] == 'cajeta', fqn
    return os.path.join(docs_root, *parts[1:-1], parts[-1] + '.md')

missing = sorted(fqn for fqn in tagged if not os.path.isfile(doc_path(fqn)))

expected = {os.path.relpath(doc_path(f), docs_root) for f in tagged}
present = set()
for dirpath, _, names in os.walk(docs_root):
    for n in names:
        if n.endswith('.md') and n != 'README.md':
            present.add(os.path.relpath(os.path.join(dirpath, n), docs_root))
unmapped = sorted(present - expected)

for fqn in missing:
    print(f"MISSING: {fqn} -> docs/stdlib/{os.path.relpath(doc_path(fqn), docs_root)}")
if unmapped:
    print(f"(informational) documents with no tagged class: {', '.join(unmapped)}")
print(f"check-stdlib-coverage: {len(tagged) - len(missing)}/{len(tagged)} tagged classes documented")
sys.exit(1 if missing else 0)
EOF
