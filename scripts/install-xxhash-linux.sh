#!/usr/bin/env bash
# Install the xxhash development headers needed by runtime/native/cajeta_runtime.c
# (it does `#define XXH_INLINE_ALL` then `#include <xxhash.h>`, so only the
# headers are required — no .so link step).
#
# Tries apt, dnf, pacman, zypper, apk in that order. Falls back to fetching the
# header set from the upstream GitHub release and dropping it under /usr/local/include.

set -euo pipefail

need_sudo() {
    if [[ $EUID -ne 0 ]]; then
        if command -v sudo >/dev/null 2>&1; then echo "sudo"; else echo ""; fi
    else
        echo ""
    fi
}

SUDO="$(need_sudo)"

try_apt() {
    command -v apt-get >/dev/null 2>&1 || return 1
    echo "Detected apt — installing libxxhash-dev."
    $SUDO apt-get update
    $SUDO apt-get install -y libxxhash-dev
}

try_dnf() {
    command -v dnf >/dev/null 2>&1 || return 1
    echo "Detected dnf — installing xxhash-devel."
    $SUDO dnf install -y xxhash-devel
}

try_yum() {
    command -v yum >/dev/null 2>&1 || return 1
    echo "Detected yum — installing xxhash-devel."
    $SUDO yum install -y xxhash-devel
}

try_pacman() {
    command -v pacman >/dev/null 2>&1 || return 1
    echo "Detected pacman — installing xxhash."
    $SUDO pacman -Sy --noconfirm xxhash
}

try_zypper() {
    command -v zypper >/dev/null 2>&1 || return 1
    echo "Detected zypper — installing xxhash-devel."
    $SUDO zypper install -y xxhash-devel
}

try_apk() {
    command -v apk >/dev/null 2>&1 || return 1
    echo "Detected apk — installing xxhash-dev."
    $SUDO apk add --no-cache xxhash-dev
}

try_source_fallback() {
    local version="${XXHASH_VERSION:-0.8.3}"
    local prefix="${XXHASH_PREFIX:-/usr/local}"
    local tmp
    tmp="$(mktemp -d)"
    trap 'rm -rf "$tmp"' RETURN

    echo "No supported package manager found — fetching xxhash ${version} from source."
    if ! command -v curl >/dev/null 2>&1; then
        echo "error: curl is required for the source fallback. Install curl and re-run." >&2
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

if try_apt || try_dnf || try_yum || try_pacman || try_zypper || try_apk; then
    echo "xxhash dev headers installed."
else
    try_source_fallback
fi

# Sanity-check.
if printf '#include <xxhash.h>\nint main(void){return 0;}\n' \
   | ${CC:-cc} -E -x c - >/dev/null 2>&1; then
    echo "ok: <xxhash.h> resolves on the default include path."
else
    echo "warning: <xxhash.h> did not resolve via ${CC:-cc} -E. You may need to pass" >&2
    echo "         -I/usr/local/include (or wherever the headers landed)." >&2
fi
