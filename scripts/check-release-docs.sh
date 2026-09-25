#!/usr/bin/env bash
# Self-test for scripts/update-release-docs.sh
# (release-download-surface plan 1.1.1 through 1.1.5).
#
# Builds a fixture release tree and asserts the rendered README block against
# what the release actually carries. The fixture deliberately gives one triple
# no installer, because "a leg shipped no installer" is the normal case today:
# the installer steps are non-fatal and a missing one must render as absent
# rather than take the row or the run down.
set -uo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)"
ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
# Repo-local scratch. /tmp is off limits here, and tmp/ is gitignored so it
# does NOT exist in a fresh clone: mktemp into a missing parent fails, $TMP
# comes back empty, and every later `cd "$TMP"` lands in the repo root.
# The renderer replaces marker blocks with python3, and so do this test's
# manifest assertions. The release job runs on a host that has it; a C++
# build leg need not, and making a docs renderer a hard dependency of one
# would be the wrong trade. Skip loudly instead.
if ! command -v python3 >/dev/null 2>&1; then
    echo ">> check-release-docs: python3 absent — skipped"
    exit 0
fi
SCRATCH="${TMPDIR:-${ROOT}/tmp}"
mkdir -p "$SCRATCH"
TMP="$(mktemp -d "${SCRATCH}/release-docs-selftest.XXXXXX")" || exit 1
[ -n "$TMP" ] && [ -d "$TMP" ] || { echo "FAIL: no scratch dir"; exit 1; }
trap 'rm -rf "$TMP"' EXIT

fails=0
# `cmp` is not on the mingw runner. Compare contents with the shell, which
# is everywhere these render text files.
same_file() { [ "$(cat "$1")" = "$(cat "$2")" ] && echo same || echo differs; }
assert() { # <desc> <expected> <actual>
    if [ "$2" != "$3" ]; then
        echo "FAIL: $1 (expected '$2', got '$3')"
        fails=$((fails + 1))
    fi
}
assert_has() { # <desc> <file> <fixed-string>
    if grep -qF -- "$3" "$2"; then return 0; fi
    echo "FAIL: $1 — not found: $3"
    fails=$((fails + 1))
}
assert_lacks() { # <desc> <file> <fixed-string>
    if grep -qF -- "$3" "$2"; then
        echo "FAIL: $1 — unexpectedly present: $3"
        fails=$((fails + 1))
    fi
}

TAG="v9.9.9"

# ---- fixture: one artifact dir per triple, as the release job uploads them.
# The triple is carried by the DIRECTORY, which is the only place a cpack
# installer name records it: `cajeta_9.9.9_amd64.deb` names no triple.
mk() { mkdir -p "$(dirname "$1")"; : > "$1"; }
A="$TMP/artifacts"
mk "$A/cajeta-x86_64-linux-gnu/cajeta-${TAG}-x86_64-linux-gnu.tar.gz"
mk "$A/cajeta-x86_64-linux-gnu/cajeta_9.9.9_amd64.deb"
mk "$A/cajeta-x86_64-linux-gnu/cajeta-9.9.9-1.x86_64.rpm"
mk "$A/cajeta-aarch64-linux-gnu/cajeta-${TAG}-aarch64-linux-gnu.tar.gz"
mk "$A/cajeta-aarch64-apple-darwin/cajeta-${TAG}-aarch64-apple-darwin.tar.gz"
mk "$A/cajeta-aarch64-apple-darwin/cajeta-9.9.9-Darwin.pkg"
mk "$A/cajeta-x86_64-w64-mingw32/cajeta-${TAG}-x86_64-w64-mingw32.zip"
mk "$A/cajeta-x86_64-w64-mingw32/cajeta-9.9.9-win64.msi"
# cvm's OWN packages, staged where the release stages them. Same extensions
# as the compiler's, so only the directory tells them apart.
mk "$A/cajeta-x86_64-linux-gnu/cvm-dist/cajeta-cvm_9.9.9_amd64.deb"
mk "$A/cajeta-aarch64-apple-darwin/cvm-dist/cajeta-cvm-9.9.9-Darwin.pkg"

R="$TMP/release-assets"
mk "$R/cajeta-${TAG}-x86_64-linux-gnu"
mk "$R/cvm-${TAG}-x86_64-linux-gnu"
mk "$R/cajeta-${TAG}-aarch64-linux-gnu"
mk "$R/cvm-${TAG}-aarch64-linux-gnu"
mk "$R/cajeta-${TAG}-aarch64-apple-darwin"
mk "$R/cvm-${TAG}-aarch64-apple-darwin"
mk "$R/cajeta-${TAG}-x86_64-w64-mingw32.exe"
mk "$R/cvm-${TAG}-x86_64-w64-mingw32.exe"

SENTINEL_TOP="KEEP ME ABOVE THE MARKERS"
SENTINEL_BOT="KEEP ME BELOW THE MARKERS"
new_readme() {
    cat > "$1" <<EOF
# cajeta

$SENTINEL_TOP

<!-- BEGIN:RELEASE -->
stale content that must be replaced
<!-- END:RELEASE -->

$SENTINEL_BOT
EOF
}
new_guide() {
    cat > "$1" <<'EOF'
# Installation

<!-- BEGIN:CVM_DOWNLOAD -->
stale
<!-- END:CVM_DOWNLOAD -->
EOF
}

README="$TMP/README.md"; new_readme "$README"
GUIDE="$TMP/guide.md";   new_guide   "$GUIDE"
MANIFEST="$TMP/latest-release.json"

# A stand-in for cvm's catalog, which this surface must never touch (spec
# 2.4.1). Its bytes are the assertion.
CATALOG="$TMP/cvm-catalog.json"
printf '{"releases":["v0.0.1"]}\n' > "$CATALOG"
CATALOG_BEFORE="$(cat "$CATALOG")"

run() {
    ( cd "$TMP" && RELEASE_DOCS_README="$README" RELEASE_DOCS_GUIDE="$GUIDE" \
        RELEASE_DOCS_MANIFEST="$MANIFEST" \
        GITHUB_REPOSITORY="jklappenbach/cajeta" \
        "$SCRIPT_DIR/update-release-docs.sh" "$TAG" release-assets artifacts \
        >"$TMP/out.log" 2>&1 )
}

run; rc=$?
assert "1.1.1 renders a full release without error" 0 "$rc"
[ "$rc" -eq 0 ] || sed 's/^/    /' "$TMP/out.log"

# 1.1.1 every column present, and each header names what its column holds.
assert_has "1.1.1 header names the installer column" "$README" "| Installer |"
assert_has "1.1.1 header names the cvm column" "$README" "| cvm |"
assert_has "1.1.1 archive linked" "$README" "cajeta-${TAG}-x86_64-linux-gnu.tar.gz"
assert_has "1.1.1 compiler binary linked" "$README" "[binary]"
assert_has "1.1.1 cvm linked" "$README" "cvm-${TAG}-x86_64-linux-gnu"
assert_has "1.1.1 deb linked" "$README" "cajeta_9.9.9_amd64.deb"
assert_has "1.1.1 rpm linked" "$README" "cajeta-9.9.9-1.x86_64.rpm"
assert_has "1.1.1 pkg linked" "$README" "cajeta-9.9.9-Darwin.pkg"
assert_has "1.1.1 msi linked" "$README" "cajeta-9.9.9-win64.msi"

# cvm-installer 3.1.2 / 3.1.4: cvm packages get their OWN cell, and they must
# not leak into the compiler's Installer cell. They share every extension, so
# a classifier keyed on the name would put them in both.
assert_has "cvm-installer header has its own column" "$README" "| cvm installer |"
assert_has "cvm deb linked" "$README" "cajeta-cvm_9.9.9_amd64.deb"
assert_has "cvm pkg linked" "$README" "cajeta-cvm-9.9.9-Darwin.pkg"
lxrow="$(grep -F '`x86_64-linux-gnu`' "$README" | head -1)"
assert "cvm deb is NOT in the compiler installer cell" "clean" \
    "$(python3 -c "
import sys
row = sys.argv[1]
cells = [c.strip() for c in row.strip().strip(chr(124)).split(chr(124))]
# Platform, Triple, Archive, Compiler, Installer, cvm, cvm installer
print('leaked' if 'cajeta-cvm_' in cells[4] else 'clean')" "$lxrow")"
assert "the cvm installer cell HAS it" "present" \
    "$(python3 -c "
import sys
row = sys.argv[1]
cells = [c.strip() for c in row.strip().strip(chr(124)).split(chr(124))]
print('present' if 'cajeta-cvm_' in cells[6] else 'missing')" "$lxrow")"
# spec 5.4: the bare binary needs chmod and the package does not. A reader
# who is not told that downloads a file that answers permission denied.
assert_has "the block says the bare cvm needs chmod" "$README" "chmod +x"
# A triple with no cvm package renders absent rather than borrowing another's.
arow="$(grep -F '`aarch64-linux-gnu`' "$README" | head -1)"
assert "a triple with no cvm package reads absent" "absent" \
    "$(python3 -c "
import sys
row = sys.argv[1]
cells = [c.strip() for c in row.strip().strip(chr(124)).split(chr(124))]
print('absent' if cells[6] == chr(8212) else 'got:' + cells[6][:20])" "$arow")"

# 1.1.1 the cvm link must NOT be what sits under the Installer header. The
# shipped table had exactly that, so assert the ordering, not just presence.
hdr="$(grep -F '| Platform |' "$README" | head -1)"
assert "1.1.1 Installer precedes cvm in the header" \
    "ok" \
    "$(awk -v h="$hdr" 'BEGIN{i=index(h,"Installer"); c=index(h,"| cvm"); print (i>0 && c>i) ? "ok" : "no"}')"

# 1.1.2 a triple with no installer still renders, with an absent cell.
row="$(grep -F '`aarch64-linux-gnu`' "$README" | head -1)"
assert "1.1.2 the installer-less triple still has a row" "yes" \
    "$([ -n "$row" ] && echo yes || echo no)"
assert "1.1.2 its installer cell reads absent" "yes" \
    "$(printf '%s' "$row" | grep -qF -- '—' && echo yes || echo no)"

# 1.1.5 prose outside the markers survives.
assert_has "1.1.5 text above the markers survives" "$README" "$SENTINEL_TOP"
assert_has "1.1.5 text below the markers survives" "$README" "$SENTINEL_BOT"
assert_lacks "1.1.5 stale block content is gone" "$README" "stale content that must be replaced"

# ---- Unit 2: the release manifest the site imports ------------------------
jqq() { python3 -c "import json,sys; print(json.load(open(sys.argv[1]))$1)" "$MANIFEST"; }

assert "2.1.1 manifest names the version" "9.9.9" "$(jqq '["version"]')"
assert "2.1.1 manifest carries every platform that shipped" "4" \
    "$(jqq '["platforms"].__len__()')"
assert "2.1.1 an asset carries its URL" "yes" \
    "$(jqq '["platforms"][0]["compiler"]["url"].startswith("https://github.com/") and "yes" or "no"')"
assert "2.1.1 installers are listed with their format" "deb" \
    "$(python3 -c "
import json,sys
d=json.load(open('$MANIFEST'))
p=[x for x in d['platforms'] if x['triple']=='x86_64-linux-gnu'][0]
print(sorted(i['format'] for i in p['installers'])[0])")"

# cvm-installer 3.1.1: the manifest carries the cvm packages as a LIST, since a
# platform ships more than one format and a scalar keeps only the last.
assert "3.1.1 manifest lists both cvm package formats" "deb" \
    "$(python3 -c "
import json
d=json.load(open('$MANIFEST'))
p=[x for x in d['platforms'] if x['triple']=='x86_64-linux-gnu'][0]
c=p.get('cvm-installers',[])
print(sorted(i['format'] for i in c)[0] if c else 'MISSING')")"
assert "3.1.1 the cvm package is not filed as a compiler installer" "clean" \
    "$(python3 -c "
import json
d=json.load(open('$MANIFEST'))
p=[x for x in d['platforms'] if x['triple']=='x86_64-linux-gnu'][0]
print('leaked' if any('cajeta-cvm' in i['name'] for i in p.get('installers',[])) else 'clean')")"

# 2.1.2 the installer-less triple has no installers key at all, rather than an
# entry pointing at a file that was never published.
assert "2.1.2 absent assets are omitted, not recorded dead" "absent" \
    "$(python3 -c "
import json
d=json.load(open('$MANIFEST'))
p=[x for x in d['platforms'] if x['triple']=='aarch64-linux-gnu'][0]
print('absent' if 'installers' not in p else 'present')")"

# 2.1.5 cvm's catalog is a different artifact and must be untouched.
assert "2.1.5 cvm's catalog is not touched" "unchanged" \
    "$([ "$CATALOG_BEFORE" = "$(cat "$CATALOG")" ] && echo unchanged || echo changed)"

# 1.1.4 / 2.1.3 rendering twice over the same tree is byte-identical, for the
# README and for the manifest. A release commit must carry no churn.
cp "$README" "$TMP/first.md"
cp "$MANIFEST" "$TMP/first.json"
run
assert "1.1.4 second render is byte-identical" "same" \
    "$(same_file "$TMP/first.md" "$README")"
assert "2.1.3 manifest re-render is byte-identical" "same" \
    "$(same_file "$TMP/first.json" "$MANIFEST")"

# 2.1.4 the guard fires when the surface does not name what just shipped.
run_other_tag() {
    ( cd "$TMP" && RELEASE_DOCS_README="$README" RELEASE_DOCS_GUIDE="$GUIDE" \
        RELEASE_DOCS_MANIFEST="$TMP/mismatch.json" \
        GITHUB_REPOSITORY="jklappenbach/cajeta" \
        "$SCRIPT_DIR/update-release-docs.sh" "$1" release-assets artifacts \
        >"$TMP/out2.log" 2>&1 )
}
new_readme "$README"
run_other_tag "v0.0.0"; rc=$?
assert "2.1.4 a tag with no matching assets fails rather than renders" "nonzero" \
    "$([ "$rc" -ne 0 ] && echo nonzero || echo 0)"
new_readme "$README"; run >/dev/null 2>&1

# 1.1.3 a file without the marker pair fails rather than writing nothing.
printf '# no markers here\n' > "$README"
run; rc=$?
assert "1.1.3 missing markers fail the run" "nonzero" \
    "$([ "$rc" -ne 0 ] && echo nonzero || echo 0)"

if [ "$fails" -eq 0 ]; then
    echo "check-release-docs: OK"
    exit 0
fi
echo "check-release-docs: $fails assertion(s) failed"
exit 1
