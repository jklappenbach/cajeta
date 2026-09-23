#!/usr/bin/env bash
# Regenerate the release blocks in README.md and docs/guide/01-installation.md
# from a tag and the assets a release actually carries.
#
# Called by .github/workflows/release.yml after the Release is published, and
# runnable by hand to preview the result:
#
#   scripts/update-release-docs.sh v0.27.0 release-assets artifacts
#
# Both files carry paired marker comments. Everything BETWEEN a pair is
# generated and replaced wholesale on every run; everything outside is
# hand-written and never touched. Editing inside the markers is pointless —
# the next release overwrites it.
#
# The asset directories decide what gets linked, and they are searched
# recursively because the bare binaries and the archives are staged in
# different trees. A triple with no asset produces no row, so a release that
# lost a leg advertises the downloads it really has rather than four links,
# three of which 404.
set -euo pipefail

TAG="${1:?usage: update-release-docs.sh <tag> [asset-dir...]}"
shift
ASSET_DIRS=("$@")
[ ${#ASSET_DIRS[@]} -gt 0 ] || ASSET_DIRS=(release-assets)
VERSION="${TAG#v}"
REPO="${GITHUB_REPOSITORY:-jklappenbach/cajeta}"
BASE="https://github.com/${REPO}/releases/download/${TAG}"

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
# Overridable so the self-test can render into a fixture instead of the real
# files. Unset in CI and by hand, which is the path that matters.
README="${RELEASE_DOCS_README:-${ROOT}/README.md}"
GUIDE="${RELEASE_DOCS_GUIDE:-${ROOT}/docs/guide/01-installation.md}"
# The release manifest the site imports. Committed, unlike the site's own
# manifest.json, which is gitignored because it is DERIVED from ../docs and any
# build can regenerate it. This one describes a release, so nothing in the tree
# can reproduce it and the site build would otherwise need the network.
MANIFEST="${RELEASE_DOCS_MANIFEST:-${ROOT}/site/src/data/latest-release.json}"

# triple|label|archive extension — mirrors the build matrix in release.yml.
TARGETS=(
  "x86_64-linux-gnu|Linux x86_64|tar.gz"
  "aarch64-linux-gnu|Linux ARM64|tar.gz"
  "aarch64-apple-darwin|macOS Apple Silicon|tar.gz"
  "x86_64-w64-mingw32|Windows x86_64|zip"
)

# Is `$1` present anywhere under the asset directories? Exact basename match,
# so `cajeta-v1.0.0-x86_64-linux-gnu` never matches its own `.sha256` sidecar.
has_asset() {
  local name="$1" d
  for d in "${ASSET_DIRS[@]}"; do
    [ -d "$d" ] || continue
    find "$d" -type f -name "$name" -print -quit | grep -q . && return 0
  done
  return 1
}

# The bare-binary asset name for a triple, or "" when this release has none.
# Windows carries a .exe suffix; every other target is extensionless.
asset_for() {
  local prefix="$1" triple="$2" name
  for name in "${prefix}-${TAG}-${triple}" "${prefix}-${TAG}-${triple}.exe"; do
    has_asset "$name" && { printf '%s' "$name"; return 0; }
  done
  printf ''
}

# The native installers for a triple, one basename per line, or nothing.
#
# cpack names them its own way — `cajeta_0.29.1_amd64.deb`, `cajeta-0.29.1-1
# .x86_64.rpm`, `cajeta-0.29.1-Darwin.pkg` — and NONE of those records a
# triple, so they cannot be matched the way the bare binaries are. The
# uploading directory does record it: the release job uploads each leg as
# `cajeta-<triple>`, so that is what identifies the platform.
installers_for() {
  local triple="$1" d
  for d in "${ASSET_DIRS[@]}"; do
    [ -d "$d" ] || continue
    find "$d" -type f -path "*/cajeta-${triple}/*" \
      \( -name '*.deb' -o -name '*.rpm' -o -name '*.msi' \
         -o -name '*.pkg' -o -name '*.pkg.tar.zst' \) -printf '%f\n'
  done | sort -u
}

# The published digest for an asset, from the `.sha256` sidecar beside it, or
# "" when the release carries none. First field, because the sidecar is
# `sha256sum` output: digest, two spaces, filename.
digest_of() {
  local base="$1" d f
  for d in "${ASSET_DIRS[@]}"; do
    [ -d "$d" ] || continue
    f="$(find "$d" -type f -name "${base}.sha256" -print -quit)"
    [ -n "$f" ] && { awk 'NR==1 {print $1}' "$f"; return 0; }
  done
  printf ''
}

# The cell for those installers: one link per format, labelled by extension so
# a reader picks the one their system takes. Sorted, so a re-render of an
# unchanged release is byte-identical.
installer_cell() {
  local triple="$1" cell="" base ext
  while IFS= read -r base; do
    [ -n "$base" ] || continue
    case "$base" in
      *.pkg.tar.zst) ext="pkg.tar.zst" ;;
      *)             ext="${base##*.}" ;;
    esac
    cell+="[\`.${ext}\`](${BASE}/${base}) "
  done < <(installers_for "$triple")
  [ -n "$cell" ] && printf '%s' "${cell% }" || printf '—'
}

# Replace the text between BEGIN:<marker> and END:<marker> in a file. The
# markers stay; only what sits between them is rewritten. Absent markers are
# a hard error — a silent no-op here would ship stale versions forever.
replace_block() {
  local file="$1" marker="$2" body="$3"
  BODY="$body" MARKER="$marker" FILE="$file" python3 - <<'PY'
import os, re, sys
path, marker, body = os.environ["FILE"], os.environ["MARKER"], os.environ["BODY"]
text = open(path, encoding="utf-8").read()
pattern = re.compile(
    r"(<!-- BEGIN:%s[^>]*-->\n).*?(<!-- END:%s -->)" % (re.escape(marker), re.escape(marker)),
    re.DOTALL,
)
if not pattern.search(text):
    sys.exit(f"error: {path} has no BEGIN:{marker} / END:{marker} marker pair")
# `$(cat <<EOF)` eats trailing newlines, so normalize to exactly one and keep
# the END marker on a line of its own.
body = body.rstrip("\n") + "\n"
open(path, "w", encoding="utf-8").write(pattern.sub(lambda m: m.group(1) + body + m.group(2), text))
print(f"updated {marker} in {path}")
PY
}

# ---- README: version line + the download table -----------------------------

rows=""
records=""
for entry in "${TARGETS[@]}"; do
  IFS='|' read -r triple label ext <<<"$entry"
  cajeta_bin="$(asset_for cajeta "$triple")"
  cvm_bin="$(asset_for cvm "$triple")"
  inst_cell="$(installer_cell "$triple")"
  archive="cajeta-${TAG}-${triple}.${ext}"
  # A triple earns a row only on an asset that NAMES the tag. Installers are
  # found by their artifact directory, which carries no version, so they cannot
  # corroborate that this release covers this platform. Counting one on its own
  # rendered rows for a tag with no assets at all, every link a 404.
  [ -n "$cajeta_bin" ] || [ -n "$cvm_bin" ] || has_asset "$archive" || continue

  if has_asset "$archive"; then
    archive_cell="[\`.${ext}\`](${BASE}/${archive})"
  else
    archive_cell="—"
  fi
  [ -n "$cajeta_bin" ] && cajeta_cell="[binary](${BASE}/${cajeta_bin})" || cajeta_cell="—"
  [ -n "$cvm_bin" ]    && cvm_cell="[\`cvm\`](${BASE}/${cvm_bin})"     || cvm_cell="—"

  rows+="| ${label} | \`${triple}\` | ${archive_cell} | ${cajeta_cell} | ${inst_cell} | ${cvm_cell} |"$'\n'

  # The same pass feeds the manifest. One enumeration, two surfaces, so the
  # README and the home page cannot come to different conclusions about what
  # this release shipped.
  emit() { # <kind> <basename>
    [ -n "$2" ] || return 0
    records+="${triple}"$'\t'"${label}"$'\t'"$1"$'\t'"$2"$'\t'"$(digest_of "$2")"$'\n'
  }
  has_asset "$archive" && emit archive "$archive"
  emit compiler "$cajeta_bin"
  emit cvm "$cvm_bin"
  while IFS= read -r inst; do emit installer "$inst"; done < <(installers_for "$triple")
done

[ -n "$rows" ] || {
  echo "error: ${ASSET_DIRS[*]} held no asset named for ${TAG}" >&2
  exit 1
}

# ---- the release manifest the site imports ---------------------------------
# Keys sorted and platforms in matrix order, so re-rendering an unchanged
# release rewrites the same bytes and a release commit carries no churn.
mkdir -p "$(dirname "$MANIFEST")"
printf '%s' "$records" | TAG="$TAG" VERSION="$VERSION" BASE="$BASE" REPO="$REPO" \
  python3 -c '
import json, os, sys

tag, version = os.environ["TAG"], os.environ["VERSION"]
base, repo = os.environ["BASE"], os.environ["REPO"]

order, plats = [], {}
for line in sys.stdin.read().splitlines():
    if not line.strip():
        continue
    triple, label, kind, name, digest = line.split("\t")
    p = plats.get(triple)
    if p is None:
        order.append(triple)
        p = plats[triple] = {"triple": triple, "label": label, "installers": []}
    entry = {"name": name, "url": f"{base}/{name}"}
    if digest:
        entry["sha256"] = digest
    if kind == "installer":
        # Labelled by what a reader has to know to pick one.
        ext = "pkg.tar.zst" if name.endswith(".pkg.tar.zst") else name.rsplit(".", 1)[-1]
        entry["format"] = ext
        p["installers"].append(entry)
    else:
        p[kind] = entry

for p in plats.values():
    p["installers"].sort(key=lambda e: e["name"])
    if not p["installers"]:
        del p["installers"]

doc = {
    "schemaVersion": 1,
    "tag": tag,
    "version": version,
    "releaseUrl": f"https://github.com/{repo}/releases/tag/{tag}",
    "platforms": [plats[t] for t in order],
}
sys.stdout.write(json.dumps(doc, indent=2, sort_keys=True) + "\n")
' > "$MANIFEST"

# The guard of spec 5.4: a surface that does not name what just shipped is
# worse than no surface, because it keeps advertising the previous release.
if ! grep -qF "\"version\": \"${VERSION}\"" "$MANIFEST"; then
  echo "error: ${MANIFEST} does not name ${VERSION}" >&2
  exit 1
fi

readme_body=$(cat <<EOF
**Current:** \`${VERSION}\` &nbsp;·&nbsp; baked into the binary at configure time — \`cajeta --version\` reports it.

**[Release notes and every asset →](https://github.com/${REPO}/releases/tag/${TAG})**

| Platform | Triple | Archive | Compiler | Installer | cvm |
|---|---|---|---|---|---|
${rows}
Each binary is published with a matching \`.sha256\`. **Installer** is the
native package for the platform, and **Archive** is the same toolchain as a
plain tarball or zip. \`cvm\` is the toolchain manager — download it once,
then \`cvm install latest\` handles every upgrade after that.
See [Installation](docs/guide/01-installation.md).

EOF
)
replace_block "$README" "RELEASE" "$readme_body"

# ---- Installation guide: how to get cvm in the first place -----------------
# The guide told readers to run `cvm install latest` without ever saying where
# cvm comes from. These are the links that close that loop.

guide_rows=""
for entry in "${TARGETS[@]}"; do
  IFS='|' read -r triple label ext <<<"$entry"
  cvm_bin="$(asset_for cvm "$triple")"
  [ -n "$cvm_bin" ] || continue
  guide_rows+="| ${label} | \`${triple}\` | [\`${cvm_bin}\`](${BASE}/${cvm_bin}) |"$'\n'
done

if [ -n "$guide_rows" ]; then
  guide_body=$(cat <<EOF
Download the build for your platform, mark it executable, and put it on your
\`PATH\`. It is a single self-contained binary with nothing to install
alongside it.

| Platform | Triple | Download |
|---|---|---|
${guide_rows}
\`\`\`bash
curl -fsSL -o cvm ${BASE}/cvm-${TAG}-x86_64-linux-gnu
chmod +x cvm && sudo mv cvm /usr/local/bin/
\`\`\`

Verify the download against the matching \`.sha256\` published beside it.
These links point at \`${TAG}\`; the
[latest release](https://github.com/${REPO}/releases/latest) always carries
the current build.

EOF
)
else
  guide_body=$(cat <<EOF
No \`cvm\` binary shipped with \`${TAG}\`. Build it from source — see
[\`tools/cvm\`](../../tools/cvm/) — or use the
[latest release](https://github.com/${REPO}/releases/latest).

EOF
)
fi
replace_block "$GUIDE" "CVM_DOWNLOAD" "$guide_body"
