#!/usr/bin/env bash
# Install the xxhash development headers needed by runtime/native/cajeta_runtime.c
# (it does `#define XXH_INLINE_ALL` then `#include <xxhash.h>`, so only the
# headers are required — no .dylib link step).
#
# Strategy: prefer Homebrew. Fall back to MacPorts. Final fallback: fetch the
# header set from the upstream GitHub release and drop it under /usr/local/include.

set -euo pipefail

need_sudo() {
    if [[ $EUID -ne 0 ]]; then
        if command -v sudo >/dev/null 2>&1; then echo "sudo"; else echo ""; fi
    else
        echo ""
    fi
}

SUDO="$(need_sudo)"

try_brew() {
    command -v brew >/dev/null 2>&1 || return 1
    echo "Detected Homebrew — installing xxhash."
    # `brew install` is idempotent; --quiet keeps the noise down on rebuilds.
    brew install xxhash
    echo
    echo "Homebrew prefix: $(brew --prefix xxhash)"
    echo "If your toolchain doesn't already pick this up, add:"
    echo "  export CPATH=\"\$(brew --prefix)/include:\${CPATH:-}\""
    echo "  export LIBRARY_PATH=\"\$(brew --prefix)/lib:\${LIBRARY_PATH:-}\""
    echo "to your shell profile (Apple Silicon Homebrew lives under /opt/homebrew,"
    echo "which Apple clang does not search by default)."
}

try_port() {
    command -v port >/dev/null 2>&1 || return 1
    echo "Detected MacPorts — installing xxhash."
    $SUDO port install xxhash
}

try_source_fallback() {
    local version="${XXHASH_VERSION:-0.8.3}"
    local prefix="${XXHASH_PREFIX:-/usr/local}"
    local tmp
    tmp="$(mktemp -d)"
    trap 'rm -rf "$tmp"' RETURN

    echo "No package manager found — fetching xxhash ${version} from source."
    if ! command -v curl >/dev/null 2>&1; then
        echo "error: curl is required for the source fallback." >&2
        return 1
    fi
    curl -fsSL "https://github.com/Cyan4973/xxHash/archive/refs/tags/v${version}.tar.gz" \
        | tar -xz -C "$tmp"
    $SUDO install -d "${prefix}/include"
    $SUDO install -m 0644 \
        "${tmp}/xxHash-${version}/xxhash.h" \
        "${tmp}/xxHash-${version}/xxh3.h" \
        "${tmp}/xxHash-${version}/xxh_x86dispatch.h" \
        "${prefix}/include/"
    echo "Installed xxhash headers to ${prefix}/include/."
}

if try_brew || try_port; then
    echo "xxhash dev headers installed."
else
    try_source_fallback
fi

# Sanity-check using whatever cc is on PATH (xcrun clang on stock macOS).
if printf '#include <xxhash.h>\nint main(void){return 0;}\n' \
   | ${CC:-cc} -E -x c - >/dev/null 2>&1; then
    echo "ok: <xxhash.h> resolves on the default include path."
else
    echo "warning: <xxhash.h> did not resolve via ${CC:-cc} -E." >&2
    echo "         On Apple Silicon Homebrew, export CPATH=\"\$(brew --prefix)/include\"." >&2
fi
