#!/usr/bin/env bash
# Bundle a macOS cajeta binary's non-system dylibs beside it, so the release
# runs on a Mac without Homebrew.  usage:
#   macos-bundle-deps.sh <binary> <openssl-libdir>
#
# Writes <dir of binary>/cajeta-deps/: every dylib `otool -L` reaches outside
# /usr/lib and /System (transitively), with @rpath install names, plus
# libssl.a and libcrypto.a from <openssl-libdir> for the static OpenSSL link
# of a user program's --emit=exe (src/cajeta/compile/MacHostPaths.h). The
# binary's references are rewritten to @rpath and it carries the
# @executable_path/cajeta-deps rpath (CMake bakes it; added here if absent).
# install_name_tool voids an arm64 signature, so every edited file is
# re-signed ad hoc. cajeta-cloud-adapters 5.3.3, decided 2026-10-27.
set -euo pipefail

if [ "$#" -ne 2 ]; then
    echo "usage: macos-bundle-deps.sh <binary> <openssl-libdir>" >&2
    exit 2
fi
BIN="$1"
SSLDIR="$2"
DEPS="$(cd "$(dirname "$BIN")" && pwd)/cajeta-deps"
rm -rf "$DEPS"
mkdir -p "$DEPS"

# The load commands of a Mach-O that point outside the system and are not
# already relative.
foreign() {
    otool -L "$1" | tail -n +2 | awk '{print $1}' | while read -r p; do
        case "$p" in
            /usr/lib/*|/System/*|@rpath/*|@executable_path/*|@loader_path/*) ;;
            *) echo "$p" ;;
        esac
    done
}

# Copy the transitive closure, breadth-first; a library is named by its
# basename in cajeta-deps/.
queue=$(foreign "$BIN")
seen=""
while [ -n "$queue" ]; do
    next=""
    for p in $queue; do
        b=$(basename "$p")
        case " $seen " in *" $b "*) continue ;; esac
        seen="$seen $b"
        [ -f "$p" ] || { echo "macos-bundle-deps: $p (needed by the closure) is missing" >&2; exit 1; }
        cp -L "$p" "$DEPS/$b"
        chmod u+w "$DEPS/$b"
        next="$next $(foreign "$p")"
    done
    queue=$(echo $next)
done

# Rewrite: each bundled dylib's id and its own references, then the binary.
for f in "$DEPS"/*.dylib; do
    [ -e "$f" ] || continue
    install_name_tool -id "@rpath/$(basename "$f")" "$f"
    for p in $(foreign "$f"); do
        install_name_tool -change "$p" "@rpath/$(basename "$p")" "$f"
    done
done
for p in $(foreign "$BIN"); do
    install_name_tool -change "$p" "@rpath/$(basename "$p")" "$BIN"
done
if ! otool -l "$BIN" | grep -A2 LC_RPATH | grep -q "@executable_path/cajeta-deps"; then
    install_name_tool -add_rpath "@executable_path/cajeta-deps" "$BIN"
fi

# Static OpenSSL for the programs cajeta links.
for a in libssl.a libcrypto.a; do
    [ -f "$SSLDIR/$a" ] || { echo "macos-bundle-deps: no $SSLDIR/$a" >&2; exit 1; }
    cp -L "$SSLDIR/$a" "$DEPS/$a"
done

for f in "$DEPS"/*.dylib "$BIN"; do
    [ -e "$f" ] || continue
    codesign --force --sign - "$f"
done

echo "macos-bundle-deps: $(ls "$DEPS" | wc -l | tr -d ' ') file(s) in $DEPS:"
ls -la "$DEPS"
