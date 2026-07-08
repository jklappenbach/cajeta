#!/bin/bash
# One-shot setup: install system build dependencies and configure the CMake
# build under ./build. Re-running is idempotent — apt/brew skip already-
# installed packages, and cmake re-configures in place.
#
# Knobs:
#   CAJETA_LLVM_VERSION  — major LLVM version to target (default 23). Used to
#                          derive LLVM_DIR and the apt package set if neither
#                          is overridden explicitly. Bump this when moving the
#                          project's LLVM baseline.
#                          Why 23: it's the current upstream baseline, and the
#                          GPU/SPIR-V path (textures, ray query, cooperative
#                          matrix — consumed from the cajeta-llvm fork) requires
#                          it. LLVM 18 lacks znver5 host-CPU detection (reports
#                          Host CPU: (unknown)); 22 is the hard floor.
#                          Use a MAINLINE LLVM (the apt llvm-N-dev / Homebrew
#                          llvm@N packages are upstream builds), NOT a vendor
#                          fork. Vendor forks such as ROCm's /opt/rocm*/llvm are
#                          built with LLVM_ENABLE_RTTI=OFF and ship no C++
#                          typeinfo for their classes; the test JIT helper then
#                          fails to link against llvm::ErrorInfoBase. (The build
#                          guards for this — see test/CMakeLists.txt — but
#                          mainline avoids the issue entirely.)
#   LLVM_DIR             — override the LLVMConfig.cmake location (default
#                          /usr/lib/llvm-${CAJETA_LLVM_VERSION}/lib/cmake/llvm).
#   CAJETA_SKIP_DEPS=1   — skip the dependency-install phase (useful on
#                          locked-down systems / CI images that pre-stage deps).
#   CAJETA_NO_LLD=1      — don't try to install lld-<ver>-dev. --emit=exe will
#                          then fall back to printing a hint at runtime.

set -euo pipefail

# Optional per-checkout LLVM pin. If scripts/llvm-env.local.sh exists (gitignored,
# machine-specific) it is sourced here, BEFORE LLVM_DIR is defaulted below, so a
# local toolchain (e.g. the cajeta-llvm fork) wins over the stock default and over
# any vendor LLVM on PATH. Absent on CI / fresh clones, so the default path holds.
_CAJETA_SETUP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ -f "${_CAJETA_SETUP_DIR}/scripts/llvm-env.local.sh" ]]; then
    # shellcheck source=/dev/null
    source "${_CAJETA_SETUP_DIR}/scripts/llvm-env.local.sh"
fi

# Auto-detect the cajeta-llvm fork so a build NEVER silently falls back to a
# vendor LLVM (notably ROCm's /opt/rocm*/llvm, built RTTI=OFF). The fork is a
# sibling clone by workspace convention; if it's installed and LLVM_DIR wasn't
# already pinned (by the local file above or the environment), use it and put
# its bin ahead of everything so bare llvm-config/clang resolve to the fork.
# This removes the "forgot to source llvm-env.local.sh" failure mode entirely —
# the fork is found by location, not by a machine-specific script.
if [[ -z "${LLVM_DIR:-}" ]]; then
    for _fork in \
        "${CAJETA_LLVM_FORK:-}" \
        "${_CAJETA_SETUP_DIR}/../cajeta-llvm/llvm-install" \
        "${_CAJETA_SETUP_DIR}/../cajeta-llvm/build"; do
        [[ -n "$_fork" ]] || continue
        if [[ -f "${_fork}/lib/cmake/llvm/LLVMConfig.cmake" ]]; then
            _fork="$(cd "$_fork" && pwd)"
            export LLVM_DIR="${_fork}/lib/cmake/llvm"
            export CMAKE_PREFIX_PATH="${_fork}${CMAKE_PREFIX_PATH:+:${CMAKE_PREFIX_PATH}}"
            export PATH="${_fork}/bin:${PATH}"
            echo ">> setup: using cajeta-llvm fork at ${_fork}"
            break
        fi
    done
fi

LLVM_VER="${CAJETA_LLVM_VERSION:-23}"

# macOS Homebrew formula for LLVM. Homebrew keeps the newest LLVM as the
# unversioned `llvm` formula and only ships `llvm@N` for some older majors --
# there is no `llvm@23` while 23 is current. Prefer the versioned formula when
# it exists, else fall back to `llvm` (which tracks the latest, currently 23.x).
# Only meaningful on macOS; harmless elsewhere.
BREW_LLVM="llvm@${LLVM_VER}"
if [[ "$(uname -s)" == "Darwin" ]] && command -v brew >/dev/null 2>&1; then
    brew info --formula "llvm@${LLVM_VER}" >/dev/null 2>&1 || BREW_LLVM="llvm"
fi

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
            LLVM_DIR="${LLVM_DIR:-$(brew --prefix ${BREW_LLVM})/lib/cmake/llvm}"
        fi
        ;;
    *)
        # Debian/Ubuntu apt install layout.
        LLVM_DIR="${LLVM_DIR:-/usr/lib/llvm-${LLVM_VER}/lib/cmake/llvm}"
        ;;
esac

# RTTI guard. cajeta uses C++ RTTI against LLVM types (dynamic_cast on
# llvm::ErrorInfoBase, etc.), so an LLVM built LLVM_ENABLE_RTTI=OFF links but
# then fails on undefined llvm::ErrorInfoBase typeinfo — the classic symptom of
# accidentally resolving ROCm's /opt/rocm*/llvm. Detect it HERE, at configure
# time, with an actionable message instead of a cryptic link error deep into the
# build. Non-fatal if llvm-config can't be found (older layouts) — the build
# still proceeds.
_llvm_config="${LLVM_DIR%/lib/cmake/llvm}/bin/llvm-config"
if [[ -x "$_llvm_config" ]]; then
    if [[ "$("$_llvm_config" --has-rtti 2>/dev/null)" == "NO" ]]; then
        echo "error: the resolved LLVM is built RTTI=OFF and cannot link cajeta" >&2
        echo "       LLVM_DIR=${LLVM_DIR}" >&2
        echo "       ($("$_llvm_config" --version 2>/dev/null), $("$_llvm_config" --prefix 2>/dev/null))" >&2
        echo "  This is almost always a vendor LLVM (e.g. ROCm's /opt/rocm*/llvm)." >&2
        echo "  cajeta needs the RTTI-enabled cajeta-llvm fork. Fix by either:" >&2
        echo "    - cloning cajeta-llvm as a sibling (../cajeta-llvm/llvm-install), or" >&2
        echo "    - setting CAJETA_LLVM_FORK / LLVM_DIR to the fork's install, or" >&2
        echo "    - creating scripts/llvm-env.local.sh (see the .example)." >&2
        exit 1
    fi
fi

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

    # LLVM 23 isn't in every distro's stock repos yet. If the versioned llvm
    # package isn't visible to apt, add the official apt.llvm.org source for
    # this release's codename (it serves both amd64 and arm64). Skipped when
    # the package is already available -- distros that ship llvm-$LLVM_VER, or
    # an image/host that already has the repo (so this won't disturb a working
    # setup).
    if ! apt-cache show "llvm-${LLVM_VER}-dev" >/dev/null 2>&1 && [[ -r /etc/os-release ]]; then
        . /etc/os-release
        echo "[deps] llvm-${LLVM_VER}-dev not in apt; adding apt.llvm.org (${VERSION_CODENAME})"
        wget -qO- https://apt.llvm.org/llvm-snapshot.gpg.key \
            | $SUDO tee /etc/apt/trusted.gpg.d/apt.llvm.org.asc >/dev/null
        echo "deb http://apt.llvm.org/${VERSION_CODENAME}/ llvm-toolchain-${VERSION_CODENAME}-${LLVM_VER} main" \
            | $SUDO tee "/etc/apt/sources.list.d/llvm-${LLVM_VER}.list" >/dev/null
        $SUDO apt-get update
    fi

    # Package list mirrors README "Build prerequisites" plus libxxhash-dev for
    # cajeta.hash (runtime/native/cajeta_runtime.c includes <xxhash.h>).
    # Note: glog's Debian package is libgoogle-glog-dev, not libglog-dev.
    #
    # LLVM-family packages are version-suffixed via $LLVM_VER (default 23, see
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
        openjdk-21-jre
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
    echo "       openjdk-21-jre libgtest-dev libgoogle-glog-dev libzstd-dev" >&2
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
    #   LLVM_DIR="$(brew --prefix ${BREW_LLVM})/lib/cmake/llvm" ./setup.sh
    local formulas=(
        cmake ninja "${BREW_LLVM}" antlr4-cpp-runtime openjdk@21
        googletest glog zstd xxhash
    )
    echo "[deps] running: brew install ${formulas[*]}"
    brew install "${formulas[@]}"
    echo "[deps] note: set LLVM_DIR=\"\$(brew --prefix ${BREW_LLVM})/lib/cmake/llvm\" before re-running setup.sh"
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
            echo "       rest (LLVM 23, ANTLR4 runtime, glog, gtest) via vcpkg or your" >&2
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
# Forward any extra args to cmake (e.g. CI passes -DCAJETA_CLANG=... on
# Windows to pin the runtime-bitcode clang to the fork toolchain). With no
# extra args "$@" expands to nothing, so every other caller is unaffected.
cmake .. -G "Ninja" -DCMAKE_BUILD_TYPE=Debug -DLLVM_DIR="${LLVM_DIR}" "$@"
popd > /dev/null

echo
echo "Setup complete. Build with: ./build.sh"
