#!/bin/bash
# One-shot setup: install system build dependencies and configure the CMake
# build under ./build. Re-running is idempotent — apt/brew skip already-
# installed packages, and cmake re-configures in place.
#
# Knobs:
#   CAJETA_LLVM_VERSION  — major LLVM version to target (default 20). Used to
#                          derive LLVM_DIR and the apt package set if neither
#                          is overridden explicitly. Bump this when moving the
#                          project's LLVM baseline.
#                          Why 20: LLVM 18 doesn't know about Zen 5 (znver5)
#                          and reports Host CPU: (unknown); LLVM 21 introduced
#                          a wave of cajeta test regressions in vtable/drop/
#                          chained-form/with-annotation codegen (the cajeta
#                          compiler hits some LLVM API behavior that changed
#                          between 20 → 21). LLVM 20 has full znver5 support,
#                          passes everything LLVM 18 passes, and additionally
#                          fixes SpawnDrop / EncodingTypes / ViewOwning that
#                          flake on 18. It's the sweet spot until we audit
#                          our cajeta-side use of the changed LLVM-21 APIs.
#   LLVM_DIR             — override the LLVMConfig.cmake location (default
#                          /usr/lib/llvm-${CAJETA_LLVM_VERSION}/lib/cmake/llvm).
#   CAJETA_SKIP_DEPS=1   — skip the dependency-install phase (useful on
#                          locked-down systems / CI images that pre-stage deps).
#   CAJETA_NO_LLD=1      — don't try to install lld-<ver>-dev. --emit=exe will
#                          then fall back to printing a hint at runtime.

set -euo pipefail

LLVM_VER="${CAJETA_LLVM_VERSION:-20}"

# LLVM_DIR default per platform — LLVMConfig.cmake lives at a different
# canonical path on each.
case "$(uname -s)" in
    MINGW*|MSYS*)
        # MSYS2 mingw-w64 install layout. The `llvm` pacman package
        # installs everything under /mingw64/.
        LLVM_DIR="${LLVM_DIR:-/mingw64/lib/cmake/llvm}"
        ;;
    Darwin)
        # Defer to brew's prefix. Callers typically set LLVM_DIR
        # explicitly via the workflow before invoking setup.sh.
        if command -v brew >/dev/null 2>&1; then
            LLVM_DIR="${LLVM_DIR:-$(brew --prefix llvm@${LLVM_VER})/lib/cmake/llvm}"
        fi
        ;;
    *)
        # Debian/Ubuntu apt install layout.
        LLVM_DIR="${LLVM_DIR:-/usr/lib/llvm-${LLVM_VER}/lib/cmake/llvm}"
        ;;
esac

# ---------------------------------------------------------------------------
# Dependency install
# ---------------------------------------------------------------------------

need_sudo() {
    if [[ $EUID -ne 0 ]]; then
        if command -v sudo >/dev/null 2>&1; then echo "sudo"; else echo ""; fi
    else
        echo ""
    fi
}

install_linux_apt() {
    local SUDO; SUDO="$(need_sudo)"

    # Package list mirrors README "Build prerequisites" plus libxxhash-dev for
    # cajeta.hash (runtime/native/cajeta_runtime.c includes <xxhash.h>).
    # Note: glog's Debian package is libgoogle-glog-dev, not libglog-dev.
    #
    # LLVM-family packages are version-suffixed via $LLVM_VER (default 21, see
    # CAJETA_LLVM_VERSION). Each is probed via apt-cache below, so the script
    # gracefully skips any that don't exist on the current Ubuntu release
    # (e.g. lld-N-dev hasn't shipped in apt on 26.04+ for any N; the static-
    # lib lldCommon/lldELF path is then off and --emit=exe prints a hint
    # instead of linking in-process). Missing libllvm$LLVM_VER is also fine
    # on 26.04+ where the runtime lib is folded into llvm-$LLVM_VER.
    local pkgs=(
        cmake
        ninja-build
        "clang-${LLVM_VER}"
        "llvm-${LLVM_VER}-dev"
        "libllvm${LLVM_VER}"
        libantlr4-runtime-dev
        openjdk-17-jre
        libgtest-dev
        libgoogle-glog-dev
        libzstd-dev
        vim-common
        libxxhash-dev
        libssl-dev
    )
    [[ "${CAJETA_NO_LLD:-0}" == "1" ]] || pkgs+=("lld-${LLVM_VER}-dev")

    local to_install=() skipped_unavail=() skipped_present=()
    local p
    for p in "${pkgs[@]}"; do
        if dpkg -s "$p" >/dev/null 2>&1; then
            skipped_present+=("$p")
            continue
        fi
        if apt-cache show "$p" >/dev/null 2>&1; then
            to_install+=("$p")
        else
            skipped_unavail+=("$p")
        fi
    done

    if (( ${#skipped_unavail[@]} > 0 )); then
        echo "[deps] skipping (not in apt cache on this release): ${skipped_unavail[*]}"
    fi
    if (( ${#to_install[@]} == 0 )); then
        echo "[deps] all available apt packages already installed."
        return 0
    fi

    echo "[deps] installing: ${to_install[*]}"
    $SUDO apt-get update
    $SUDO DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends "${to_install[@]}"
}

install_linux_fallback() {
    # Non-apt Linux distros: hand off to whatever the user has. We don't try to
    # cover every package manager here — only apt-based Linux is officially
    # supported. The xxhash piece has its own multi-distro script.
    echo "[deps] non-apt Linux detected. Install the equivalents of:" >&2
    echo "       cmake ninja-build clang-${LLVM_VER} llvm-${LLVM_VER}-dev libantlr4-runtime-dev" >&2
    echo "       openjdk-17-jre libgtest-dev libgoogle-glog-dev libzstd-dev" >&2
    echo "       vim-common (for xxd) lld-${LLVM_VER}-dev" >&2
    echo "[deps] for xxhash specifically, you can run:" >&2
    echo "       scripts/install-xxhash-linux.sh" >&2
    return 1
}

install_macos_brew() {
    if ! command -v brew >/dev/null 2>&1; then
        echo "[deps] Homebrew not found. Install from https://brew.sh and re-run." >&2
        return 1
    fi
    # Apple clang ships with macOS, so we lean on Homebrew LLVM only for the
    # bitcode-emit step (clang in src/CMakeLists.txt is found by versioned
    # name, then plain name as a fallback). Caller may need to set LLVM_DIR
    # to the brew prefix, e.g.:
    #   LLVM_DIR="$(brew --prefix llvm@${LLVM_VER})/lib/cmake/llvm" ./setup.sh
    local formulas=(
        cmake ninja "llvm@${LLVM_VER}" antlr4-cpp-runtime openjdk@17
        googletest glog zstd xxhash
    )
    echo "[deps] running: brew install ${formulas[*]}"
    brew install "${formulas[@]}"
    echo "[deps] note: set LLVM_DIR=\"\$(brew --prefix llvm@${LLVM_VER})/lib/cmake/llvm\" before re-running setup.sh"
    echo "       if cmake can't locate LLVM."
}

install_deps() {
    if [[ "${CAJETA_SKIP_DEPS:-0}" == "1" ]]; then
        echo "[deps] CAJETA_SKIP_DEPS=1, skipping dependency install."
        return 0
    fi
    case "$(uname -s)" in
        Linux)
            if command -v apt-get >/dev/null 2>&1; then
                install_linux_apt
            else
                install_linux_fallback
            fi
            ;;
        Darwin)
            install_macos_brew
            ;;
        MINGW*|MSYS*|CYGWIN*)
            echo "[deps] Windows detected. setup.sh does not auto-install on Windows." >&2
            echo "       Use scripts/install-xxhash-windows.ps1 for xxhash; install the" >&2
            echo "       rest (LLVM 18, ANTLR4 runtime, glog, gtest) via vcpkg or your" >&2
            echo "       preferred package manager." >&2
            return 1
            ;;
        *)
            echo "[deps] unknown platform $(uname -s); skipping dependency install." >&2
            ;;
    esac
}

install_deps

# ---------------------------------------------------------------------------
# CMake configure
# ---------------------------------------------------------------------------

mkdir -p build
pushd build > /dev/null
cmake .. -G "Ninja" -DCMAKE_BUILD_TYPE=Debug -DLLVM_DIR="${LLVM_DIR}"
popd > /dev/null

echo
echo "Setup complete. Build with: ./build.sh"
