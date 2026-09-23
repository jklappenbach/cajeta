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
# Repo-local scratch. /tmp is off limits here.
TMP="$(mktemp -d "${TMPDIR:-${ROOT}/tmp}/release-docs-selftest.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

fails=0
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

run() {
    ( cd "$TMP" && RELEASE_DOCS_README="$README" RELEASE_DOCS_GUIDE="$GUIDE" \
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

# 1.1.4 rendering twice over the same tree is byte-identical.
cp "$README" "$TMP/first.md"
run
assert "1.1.4 second render is byte-identical" "same" \
    "$(cmp -s "$TMP/first.md" "$README" && echo same || echo differs)"

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
