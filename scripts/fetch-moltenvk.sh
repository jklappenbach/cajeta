#!/usr/bin/env bash
# Vendor MoltenVK's prebuilt STATIC xcframework for the iOS/tvOS xpu backend
# (apple-vulkan spec 4.2, 5.3). Those platforms have no Vulkan loader, so the
# ICD is linked in rather than discovered; src/CMakeLists.txt picks the slice
# by target triple out of what this drops in External/MoltenVK.
#
# Pinned deliberately: MoltenVK tags 2-4x/year and a floating version would
# change the shader translator under a released artifact.

set -euo pipefail

VERSION="${CAJETA_MOLTENVK_VERSION:-1.4.2}"
DEST="${1:-$(cd "$(dirname "$0")/.." && pwd)/External/MoltenVK}"
URL="https://github.com/KhronosGroup/MoltenVK/releases/download/v${VERSION}/MoltenVK-all.tar"

if [[ -d "$DEST/static/MoltenVK.xcframework" ]]; then
    echo "MoltenVK already vendored at $DEST (v${VERSION} expected)"
    exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "Fetching MoltenVK v${VERSION}..."
curl -fsSL "$URL" -o "$TMP/MoltenVK-all.tar"
tar -xf "$TMP/MoltenVK-all.tar" -C "$TMP"

# The tarball unpacks to MoltenVK/ containing static/ and dynamic/. We keep only
# static/: App Store submission rejects naked dylibs (ITMS-90171).
SRC="$(find "$TMP" -maxdepth 2 -type d -name static -path '*MoltenVK*' | head -1)"
[[ -n "$SRC" ]] || { echo "error: no static/ in the MoltenVK release" >&2; exit 1; }

mkdir -p "$DEST"
rm -rf "$DEST/static"
cp -R "$SRC" "$DEST/static"
echo "$VERSION" > "$DEST/VERSION"

echo "MoltenVK v${VERSION} -> $DEST/static/MoltenVK.xcframework"
ls "$DEST/static/MoltenVK.xcframework"
