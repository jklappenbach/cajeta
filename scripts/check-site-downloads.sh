#!/usr/bin/env bash
# Self-test for the cajeta.dev downloads section
# (release-download-surface plan 3.1.1 through 3.1.4).
#
# Builds the site against a FIXTURE release manifest and asserts the rendered
# page against it: a card per platform, no link the manifest does not carry,
# an absent asset rendered as absent rather than guessed, and two builds of the
# same manifest producing the same bytes.
#
# Skips loudly where the site toolchain is absent. The C++ test host does not
# need node, and a skip that announces itself is better than a check that
# quietly is not one.
set -uo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)"
ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
SITE="${ROOT}/site"
DATA="${SITE}/src/data/latest-release.json"

if ! command -v npm >/dev/null 2>&1 || [ ! -d "${SITE}/node_modules" ]; then
    echo ">> check-site-downloads: site toolchain absent (npm or node_modules) — skipped"
    exit 0
fi

fails=0
# tmp/ is gitignored and absent in a fresh clone, so create it before any
# mktemp into it. `cmp` is not on the mingw runner either.
SCRATCH="${TMPDIR:-${ROOT}/tmp}"
mkdir -p "$SCRATCH"
same_file() { [ "$(cat "$1")" = "$(cat "$2")" ] && echo same || echo differs; }
assert() { # <desc> <expected> <actual>
    if [ "$2" != "$3" ]; then
        echo "FAIL: $1 (expected '$2', got '$3')"
        fails=$((fails + 1))
    fi
}

BACKUP=""
if [ -f "$DATA" ]; then
    BACKUP="$(mktemp "${SCRATCH}/latest-release.XXXXXX.json")"
    cp "$DATA" "$BACKUP"
fi
restore() {
    if [ -n "$BACKUP" ]; then cp "$BACKUP" "$DATA"; rm -f "$BACKUP"; fi
}
trap restore EXIT

# Fixture: one platform with everything, one with an archive only. The second
# is the point — a release where a leg shipped no installer and no cvm must
# render that platform without inventing either.
cat > "$DATA" <<'JSON'
{
  "schemaVersion": 1,
  "tag": "v9.9.9",
  "version": "9.9.9",
  "releaseUrl": "https://github.com/jklappenbach/cajeta/releases/tag/v9.9.9",
  "platforms": [
    {
      "triple": "x86_64-linux-gnu",
      "label": "Linux x86_64",
      "archive": { "name": "a.tar.gz", "url": "https://example.invalid/a.tar.gz" },
      "compiler": { "name": "c", "url": "https://example.invalid/c" },
      "cvm": { "name": "v", "url": "https://example.invalid/v" },
      "installers": [
        { "name": "p.deb", "url": "https://example.invalid/p.deb", "format": "deb" }
      ]
    },
    {
      "triple": "aarch64-apple-darwin",
      "label": "macOS Apple Silicon",
      "archive": { "name": "b.tar.gz", "url": "https://example.invalid/b.tar.gz" }
    }
  ]
}
JSON

build() { ( cd "$SITE" && npm run build >"${SCRATCH}/site-build.log" 2>&1 ); }

if ! build; then
    echo "FAIL: site build failed"
    tail -20 "${SCRATCH}/site-build.log" | sed 's/^/    /'
    exit 1
fi
PAGE="${SITE}/dist/index.html"

# 3.1.1 a card per platform in the fixture.
assert "3.1.1 a card per platform" "2" \
    "$(grep -o 'class="dl-card"' "$PAGE" | wc -l | tr -d ' ')"
assert "3.1.1 the version is named" "yes" \
    "$(grep -qF 'Download 9.9.9' "$PAGE" && echo yes || echo no)"

# 3.1.4 every platform reachable, not just the one that gets promoted.
assert "3.1.4 linux card present" "yes" \
    "$(grep -qF 'data-platform="linux-x86"' "$PAGE" && echo yes || echo no)"
assert "3.1.4 macos card present" "yes" \
    "$(grep -qF 'data-platform="mac"' "$PAGE" && echo yes || echo no)"

# 3.1.2 every rendered asset link comes FROM the manifest, and the
# archive-only platform gets no invented installer or cvm link.
assert "3.1.2 no link the manifest does not carry" "0" \
    "$(python3 - "$DATA" "$PAGE" <<'PY'
import json, re, sys
d = json.load(open(sys.argv[1]))
allowed = set()
for p in d["platforms"]:
    for k in ("archive", "compiler", "cvm"):
        if k in p: allowed.add(p[k]["url"])
    for i in p.get("installers", []): allowed.add(i["url"])
html = open(sys.argv[2], encoding="utf-8").read()
found = set(re.findall(r'https://example\.invalid/[^"\'<> ]+', html))
print(len(found - allowed))
PY
)"
assert "3.1.2 all manifest links rendered" "0" \
    "$(python3 - "$DATA" "$PAGE" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
want = set()
for p in d["platforms"]:
    for k in ("archive", "compiler", "cvm"):
        if k in p: want.add(p[k]["url"])
    for i in p.get("installers", []): want.add(i["url"])
html = open(sys.argv[2], encoding="utf-8").read()
print(len([u for u in want if u not in html]))
PY
)"

# 3.1.3 the same manifest builds the same page.
cp "$PAGE" "${SCRATCH}/site-first.html"
if ! build; then
    echo "FAIL: second site build failed"
    fails=$((fails + 1))
else
    assert "3.1.3 rebuild is byte-identical" "same" \
        "$(same_file "${SCRATCH}/site-first.html" "$PAGE")"
fi
rm -f "${SCRATCH}/site-first.html" "${SCRATCH}/site-build.log"

if [ "$fails" -eq 0 ]; then
    echo "check-site-downloads: OK"
    exit 0
fi
echo "check-site-downloads: $fails assertion(s) failed"
exit 1
