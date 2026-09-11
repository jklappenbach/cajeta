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
README="${ROOT}/README.md"
GUIDE="${ROOT}/docs/guide/01-installation.md"

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
for entry in "${TARGETS[@]}"; do
  IFS='|' read -r triple label ext <<<"$entry"
  cajeta_bin="$(asset_for cajeta "$triple")"
  cvm_bin="$(asset_for cvm "$triple")"
  [ -n "$cajeta_bin" ] || [ -n "$cvm_bin" ] || continue

  archive="cajeta-${TAG}-${triple}.${ext}"
  if has_asset "$archive"; then
    archive_cell="[\`.${ext}\`](${BASE}/${archive})"
  else
    archive_cell="—"
  fi
  [ -n "$cajeta_bin" ] && cajeta_cell="[binary](${BASE}/${cajeta_bin})" || cajeta_cell="—"
  [ -n "$cvm_bin" ]    && cvm_cell="[\`cvm\`](${BASE}/${cvm_bin})"     || cvm_cell="—"

  rows+="| ${label} | \`${triple}\` | ${archive_cell} | ${cajeta_cell} | ${cvm_cell} |"$'\n'
done

[ -n "$rows" ] || {
  echo "error: ${ASSET_DIRS[*]} held no asset named for ${TAG}" >&2
  exit 1
}

readme_body=$(cat <<EOF
**Current:** \`${VERSION}\` &nbsp;·&nbsp; baked into the binary at configure time — \`cajeta --version\` reports it.

**[Release notes and every asset →](https://github.com/${REPO}/releases/tag/${TAG})**

| Platform | Triple | Archive | Compiler | Installer |
|---|---|---|---|---|
${rows}
Each binary is published with a matching \`.sha256\`. \`cvm\` is the toolchain
manager — download it once, then \`cvm install latest\` handles every upgrade
after that. See [Installation](docs/guide/01-installation.md).

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
