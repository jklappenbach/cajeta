#!/usr/bin/env bash
# Competitor provisioning per machine (benchmark-reference spec §8.7, plan Unit 20).
#
# Installs or verifies every competitor toolchain and pinned library the spec
# names, then writes competitors/toolchains-<machine>.tsv:
#
#   item  kind (toolchain|library|checkout)  version  path  verified
#
# `version` is the version or commit found, or one of:
#   missing                 a pin that could not be found or installed — exit 1
#   missing (needs sudo)    the install needs a system package; never attempted,
#                           listed at the end, exit stays 0
#   n/a (...)               not applicable on this machine (e.g. ROCm on an NVIDIA box)
#   skipped (...)           deliberately skipped (CAJETA_SETUP_SKIP_TORCH=1)
#
# Usage:
#   setup.sh               install what is missing (user-level only), then verify
#   setup.sh --verify      verify only; install nothing
#   setup.sh --list-items  print every item name, one per line
#
# Environment:
#   CAJETA_SETUP_SKIP_TORCH=1        skip the torch wheel (multi-GB download)
#   CAJETA_SETUP_INJECT_MISSING=a,b  treat these items as missing (smoke test)
#   CAJETA_SETUP_TSV=path            write the TSV here instead
#   CAJETA_SETUP_MACHINE=name        machine name (default: hostname, lowercased)
#   CAJETA_SETUP_STEP_CAP=secs       foreground cap per install step (default 1800)
#   CAJETA_SETUP_TORCH_INDEX=url     override the torch wheel index
#   CAJETA_LLVM_BIN, CAJETA_LLAMA_CPP  fork clang bin dir, llama.cpp checkout
#
# Install locations (all user-level; bin/, logs/, third_party/, python/.venv/ are
# gitignored): Rust in ~/.cargo (rustup), oha in ~/.cargo/bin, .NET in ~/.dotnet,
# wrk in competitors/bin, the Python venv in competitors/python/.venv, pinned
# sources and jars in competitors/third_party. Each install step logs to
# competitors/logs/<step>.log and is capped at CAJETA_SETUP_STEP_CAP seconds.
#
# Nothing here touches a GPU: CUDA and ROCm are detected from files and compiler
# version strings (never nvidia-smi or rocminfo), torch is checked through its
# wheel metadata, not imported. Device suites are fetched, never built.
#
# Per-machine notes:
#   WSL2 (Phoenix): the CPU governor is not visible; /usr/lib/wsl/lib carries the
#     NVIDIA driver; .NET needs libicu (present on Ubuntu 24.04). The distro
#     clang is 14 — too old — so C++ uses g++ 13 or the cajeta-llvm fork clang.
#   Proton (Ubuntu 26.04, ROCm): torch comes from the ROCm wheel index, picked
#     from /opt/rocm/.info/version; CUDA rows read n/a.
set -uo pipefail

SELF="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")"
HERE="$(dirname "$SELF")"
REPO="$(cd "$HERE/../.." && pwd)"
MACHINE="${CAJETA_SETUP_MACHINE:-$(hostname | tr '[:upper:]' '[:lower:]')}"
TSV="${CAJETA_SETUP_TSV:-$HERE/toolchains-${MACHINE}.tsv}"
PINS="$HERE/pins.tsv"
TP="$HERE/third_party"
BIN="$HERE/bin"
LOGS="$HERE/logs"
VENV="$HERE/python/.venv"
REQS="$HERE/python/requirements.txt"
LLVM_BIN="${CAJETA_LLVM_BIN:-$REPO/../cajeta-llvm/llvm-install/bin}"
LLAMA="${CAJETA_LLAMA_CPP:-$REPO/../llama.cpp}"
STEP_CAP="${CAJETA_SETUP_STEP_CAP:-1800}"

# The existing pins (manifest.tsv + <name>.lock) live wherever vendor.sh lives:
# competitors/vendor/ once samples/profile has moved into bench/, before that
# under samples/profile.
VENDOR_DIR=""
for d in "$HERE/vendor" "$REPO/samples/profile/competitors/vendor"; do
    [[ -f "$d/manifest.tsv" ]] && { VENDOR_DIR="$d"; break; }
done

# ---- toolchain pins ---------------------------------------------------------
RUST_CHANNEL=stable
OHA_VERSION=1.16.0
WRK_TAG=4.2.0
WRK_COMMIT=a211dd5a7050b1f9e8a9870b95513060e72ac4a0
DOTNET_CHANNEL=10.0
GXX_MIN=13
CLANG_FORK_MAJOR=23
GO_MIN=1.22
JAVA_MAJOR=25
NODE_MIN=22
PYTHON_MM=3.12
TORCH_VERSION=2.14.1     # the wheel tag (+cu130 / +rocmX.Y / +cpu) is picked per machine

export PATH="$HOME/.cargo/bin:$HOME/.dotnet:$BIN:$PATH"
export DOTNET_ROOT="$HOME/.dotnet" DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1
export CARGO_HOME="${CARGO_HOME:-$HOME/.cargo}" RUSTUP_HOME="${RUSTUP_HOME:-$HOME/.rustup}"

# ---- helpers ----------------------------------------------------------------
first_line() { head -1 | tr '\t\r' '  '; }
# vge A B: version A >= version B
vge() { [[ "$(printf '%s\n%s\n' "$2" "$1" | sort -V | head -1)" == "$2" ]]; }
out() { printf '%s\t%s\t%s\n' "$1" "$2" "$3"; }   # version, path, how-verified

git_fetch_pin() {  # dest url commit
    local dest="$1" url="$2" commit="$3"
    mkdir -p "$dest"
    [[ -d "$dest/.git" ]] || git -C "$dest" init -q
    git -C "$dest" remote remove origin 2>/dev/null
    git -C "$dest" remote add origin "$url"
    git -C "$dest" fetch -q --depth 1 origin "$commit" || return 1
    git -C "$dest" -c advice.detachedHead=false checkout -q --force FETCH_HEAD
}
git_check_pin() {  # dest commit label
    local dest="$1" want="$2" have
    [[ -d "$dest/.git" ]] || return 1
    have="$(git -C "$dest" rev-parse HEAD 2>/dev/null)" || return 1
    [[ "$have" == "$want" ]] || return 1
    out "$have" "$dest" "git rev-parse HEAD == $3"
}
sha_of() { sha256sum "$1" 2>/dev/null | cut -d' ' -f1; }
pin_field() { awk -F'\t' -v n="$1" -v c="$2" '!/^#/ && $1==n {print $c; exit}' "$PINS"; }

# ---- toolchain checks / installs ---------------------------------------------
check_gxx() {
    command -v g++ >/dev/null || return 1
    local v; v="$(g++ -dumpfullversion)"; vge "$v" "$GXX_MIN" || return 1
    out "$v" "$(command -v g++)" "g++ -dumpfullversion >= $GXX_MIN"
}
prereq_gxx() { echo "apt install g++-$GXX_MIN"; return 1; }

check_clang_fork() {
    [[ -x "$LLVM_BIN/clang" ]] || return 1
    local v; v="$("$LLVM_BIN/clang" --version | first_line)"
    [[ "$v" =~ clang\ version\ ([0-9]+) ]] && (( BASH_REMATCH[1] >= CLANG_FORK_MAJOR )) || return 1
    out "$v" "$(cd "$LLVM_BIN" && pwd)/clang" "clang --version major >= $CLANG_FORK_MAJOR"
}

check_cmake() {
    command -v cmake >/dev/null || return 1
    out "$(cmake --version | first_line | sed 's/cmake version //')" "$(command -v cmake)" "cmake --version"
}
prereq_cmake() { echo "apt install cmake"; return 1; }

check_rustc() {
    [[ -x "$CARGO_HOME/bin/rustc" ]] || return 1
    out "$("$CARGO_HOME/bin/rustc" --version | first_line)" "$CARGO_HOME/bin/rustc" "rustc --version (rustup $RUST_CHANNEL)"
}
check_cargo() {
    [[ -x "$CARGO_HOME/bin/cargo" ]] || return 1
    out "$("$CARGO_HOME/bin/cargo" --version | first_line)" "$CARGO_HOME/bin/cargo" "cargo --version"
}
install_rust() {
    curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs -o "$TP/rustup-init.sh" &&
        sh "$TP/rustup-init.sh" -y --profile minimal --default-toolchain "$RUST_CHANNEL" --no-modify-path &&
        "$CARGO_HOME/bin/rustup" update "$RUST_CHANNEL"
}

check_oha() {
    [[ -x "$CARGO_HOME/bin/oha" ]] || return 1
    local v; v="$("$CARGO_HOME/bin/oha" --version | first_line)"
    [[ "$v" == *"$OHA_VERSION"* ]] || return 1
    out "$v" "$CARGO_HOME/bin/oha" "oha --version == $OHA_VERSION"
}
install_oha() {
    [[ -x "$CARGO_HOME/bin/cargo" ]] || { echo "cargo missing"; return 1; }
    "$CARGO_HOME/bin/cargo" install oha --version "$OHA_VERSION" --locked
}

check_go() {
    command -v go >/dev/null || return 1
    local v; v="$(go env GOVERSION)"; vge "${v#go}" "$GO_MIN" || return 1
    out "$v" "$(command -v go)" "go env GOVERSION >= go$GO_MIN"
}
prereq_go() { echo "apt install golang-go (>= $GO_MIN)"; return 1; }

java_major() { "$1" -version 2>&1 | first_line | sed -E 's/.*version "([0-9]+).*/\1/'; }
check_java() {
    command -v java >/dev/null || return 1
    [[ "$(java_major java)" == "$JAVA_MAJOR" ]] || return 1
    out "$(java -version 2>&1 | first_line)" "$(command -v java)" "java -version major == $JAVA_MAJOR"
}
check_javac() {
    command -v javac >/dev/null || return 1
    local v; v="$(javac -version 2>&1 | first_line)"
    [[ "$v" =~ javac\ $JAVA_MAJOR ]] || return 1
    out "$v" "$(command -v javac)" "javac -version major == $JAVA_MAJOR"
}
check_maven() {
    command -v mvn >/dev/null || return 1
    out "$(mvn -v 2>/dev/null | first_line | sed -E 's/\x1b\[[0-9;]*m//g')" "$(command -v mvn)" "mvn -v (Spring Boot, Quarkus, Micronaut builds)"
}
prereq_maven() { echo "apt install maven"; return 1; }

check_node() {
    command -v node >/dev/null || return 1
    local v; v="$(node --version)"; vge "${v#v}" "$NODE_MIN" || return 1
    out "$v" "$(command -v node)" "node --version >= v$NODE_MIN"
}

check_python3() {
    command -v python3 >/dev/null || return 1
    local v; v="$(python3 -c 'import platform; print(platform.python_version())')"
    [[ "$v" == "$PYTHON_MM".* ]] || return 1
    python3 -c 'import venv, ensurepip' 2>/dev/null || return 1
    out "$v" "$(python3 -c 'import sys; print(sys.executable)')" "python3 version $PYTHON_MM.x with venv+ensurepip"
}
prereq_python3() {
    python3 -c 'import venv, ensurepip' 2>/dev/null && return 0
    echo "apt install python$PYTHON_MM python$PYTHON_MM-venv"; return 1
}

check_venv() {
    [[ -x "$VENV/bin/python" ]] || return 1
    out "$("$VENV/bin/python" -c 'import platform; print(platform.python_version())')" "$VENV" "venv python runs"
}
install_venv() { python3 -m venv "$VENV" && "$VENV/bin/python" -m pip install -q --upgrade pip; }

py_dist_version() {
    "$VENV/bin/python" - "$1" <<'PY' 2>/dev/null
import sys, importlib.metadata as m
print(m.version(sys.argv[1]))
PY
}
check_pypkg() {  # dist
    [[ -x "$VENV/bin/python" ]] || return 1
    local v; v="$(py_dist_version "$1")" && [[ -n "$v" ]] || return 1
    local want; want="$(sed -nE "s/^$1==([^ ]+).*/\1/p" "$REQS")"
    [[ -z "$want" || "$v" == "$want" ]] || return 1
    out "$v" "$VENV" "importlib.metadata.version${want:+ == requirements.txt pin}"
}
install_pypkgs() {
    [[ -x "$VENV/bin/python" ]] || install_venv || return 1
    "$VENV/bin/python" -m pip install --upgrade -r "$REQS"
}

torch_flavor() {  # prints "<index-url> <label>"; reads files and compiler strings only
    if [[ -n "${CAJETA_SETUP_TORCH_INDEX:-}" ]]; then echo "$CAJETA_SETUP_TORCH_INDEX override"; return; fi
    if command -v rocminfo >/dev/null 2>&1 || [[ -f /opt/rocm/.info/version ]]; then
        local r; r="$(cut -d. -f1-2 /opt/rocm/.info/version 2>/dev/null)"
        echo "https://download.pytorch.org/whl/rocm${r:-6.4} rocm${r:-6.4}"; return
    fi
    local cuda=""
    if [[ -f /usr/local/cuda/version.json ]]; then
        cuda="$(python3 -c 'import json; print(json.load(open("/usr/local/cuda/version.json"))["cuda"]["version"])' 2>/dev/null)"
    elif command -v nvcc >/dev/null; then
        cuda="$(nvcc --version | sed -nE 's/.*release ([0-9.]+),.*/\1/p')"
    fi
    case "$cuda" in
        13.*) echo "https://download.pytorch.org/whl/cu130 cu130" ;;
        12.*) echo "https://download.pytorch.org/whl/cu128 cu128" ;;
        *)    echo "https://download.pytorch.org/whl/cpu cpu" ;;
    esac
}
check_torch() {
    [[ -x "$VENV/bin/python" ]] || return 1
    local v; v="$(py_dist_version torch)" && [[ -n "$v" ]] || return 1
    local label; label="$(torch_flavor | cut -d' ' -f2)"
    [[ "$v" == "$TORCH_VERSION" || "$v" == "$TORCH_VERSION+"* ]] || return 1
    [[ "$label" == override || "$label" == cpu || "$v" == *"+$label"* ]] || return 1
    out "$v" "$VENV" "importlib.metadata.version (wheel tag +$label; not imported, GPU untouched)"
}
install_torch() {
    [[ -x "$VENV/bin/python" ]] || install_venv || return 1
    local idx; idx="$(torch_flavor | cut -d' ' -f1)"
    echo "torch index: $idx"
    "$VENV/bin/python" -m pip install "torch==$TORCH_VERSION" --index-url "$idx"
}

check_dotnet() {
    [[ -x "$HOME/.dotnet/dotnet" ]] || return 1
    local v; v="$("$HOME/.dotnet/dotnet" --list-sdks 2>/dev/null | awk -v c="$DOTNET_CHANNEL." 'index($1,c)==1 {v=$1} END {print v}')"
    [[ -n "$v" ]] || return 1
    out "sdk $v" "$HOME/.dotnet/dotnet" "dotnet --list-sdks has $DOTNET_CHANNEL.x (ASP.NET Core)"
}
install_dotnet() {
    curl -sSfL https://dot.net/v1/dotnet-install.sh -o "$TP/dotnet-install.sh" &&
        bash "$TP/dotnet-install.sh" --channel "$DOTNET_CHANNEL" --install-dir "$HOME/.dotnet" --no-path
}
prereq_dotnet() {
    ldconfig -p 2>/dev/null | grep -q 'libicuuc\.so' && return 0
    echo "apt install libicu-dev (the .NET runtime needs ICU)"; return 1
}

check_wrk() {
    [[ -x "$BIN/wrk" ]] || return 1
    local v; v="$("$BIN/wrk" --version 2>&1 | first_line)"
    # A build from a git checkout reports the short commit, not the tag.
    [[ "$v" == "wrk $WRK_TAG "* || "$v" == "wrk ${WRK_COMMIT:0:7} "* ]] || return 1
    out "$WRK_TAG @ $WRK_COMMIT" "$BIN/wrk" "wrk --version reports ${WRK_COMMIT:0:7} (tag $WRK_TAG)"
}
install_wrk() {
    git_fetch_pin "$TP/wrk" https://github.com/wg/wrk "$WRK_COMMIT" || return 1
    make -C "$TP/wrk" -j"$(nproc)" WITH_OPENSSL=/usr || return 1
    mkdir -p "$BIN" && cp "$TP/wrk/wrk" "$BIN/wrk"
}
prereq_wrk() {
    local need=()
    command -v make >/dev/null || need+=(make)
    command -v cc   >/dev/null || need+=(gcc)
    [[ -f /usr/include/openssl/ssl.h ]] || need+=(libssl-dev)
    command -v unzip >/dev/null || need+=(unzip)
    (( ${#need[@]} == 0 )) && return 0
    echo "apt install ${need[*]} (to build wrk)"; return 1
}

check_cuda() {
    local nvcc="${CUDA_PATH:-/usr/local/cuda}/bin/nvcc"
    [[ -x "$nvcc" ]] || nvcc="$(command -v nvcc)" || return 2
    local v; v="$("$nvcc" --version | sed -nE 's/.*release ([0-9.]+), (V[0-9.]+).*/\1 \2/p')"
    [[ -n "$v" ]] || return 1
    out "$v" "$nvcc" "nvcc --version (no device query)"
}
check_rocm() {
    command -v hipcc >/dev/null || [[ -x /opt/rocm/bin/hipcc ]] || return 2
    local h; h="$(command -v hipcc || echo /opt/rocm/bin/hipcc)"
    local v; v="$(cat /opt/rocm/.info/version 2>/dev/null || "$h" --version 2>/dev/null | grep -m1 -i hip | tr -d '\t')"
    out "${v:-unknown}" "$h" "/opt/rocm/.info/version (no device query)"
}

check_llama() {
    [[ -d "$LLAMA/.git" || -f "$LLAMA/.git" ]] || return 1
    local c; c="$(git -C "$LLAMA" rev-parse HEAD)" || return 1
    local built=""
    for b in build-cuda build-hip build-vulkan build; do
        [[ -x "$LLAMA/$b/bin/llama-bench" ]] && built+="${built:+,}$b"
    done
    out "$c (llama-bench built: ${built:-none})" "$(cd "$LLAMA" && pwd)" "git rev-parse HEAD; ls */bin/llama-bench (not built here)"
}

# ---- pinned libraries (pins.tsv) ----------------------------------------------
lock_of() { [[ -n "$VENDOR_DIR" && -f "$VENDOR_DIR/$1.lock" ]] && head -1 "$VENDOR_DIR/$1.lock"; }
url_of_vendor() { awk -F'\t' -v n="$1" '!/^#/ && $1==n {print $2; exit}' "$VENDOR_DIR/manifest.tsv"; }

check_pin() {  # item
    local item="$1" method src pin chk
    method="$(pin_field "$item" 3)"; src="$(pin_field "$item" 4)"; pin="$(pin_field "$item" 5)"; chk="$(pin_field "$item" 6)"
    case "$method" in
        vendorlock)
            local c; c="$(lock_of "$src")" && [[ -n "$c" ]] || return 1
            git_check_pin "$TP/$item" "$c" "vendor/$src.lock" ;;
        git)
            git_check_pin "$TP/$item" "$chk" "pins.tsv ($pin)" | awk -F'\t' -v p="$pin" '{OFS="\t"; $1=p" @ "$1; print}'
            return "${PIPESTATUS[0]}" ;;
        jar)
            local f="$TP/java/$src"
            [[ "$(sha_of "$f")" == "$chk" ]] || return 1
            out "$pin" "$f" "sha256 == pins.tsv (${chk:0:12}…), from https://repo1.maven.org/maven2/$src" ;;
        tarball)
            local f="$TP/$(basename "$src")"
            [[ "$(sha_of "$f")" == "$chk" && -d "${f%.tar.gz}" ]] || return 1
            out "$pin" "${f%.tar.gz}" "sha256 of $(basename "$src") == pins.tsv (${chk:0:12}…)" ;;
        *) return 1 ;;
    esac
}
install_pin() {  # item
    local item="$1" method src pin chk
    method="$(pin_field "$item" 3)"; src="$(pin_field "$item" 4)"; pin="$(pin_field "$item" 5)"; chk="$(pin_field "$item" 6)"
    case "$method" in
        vendorlock)
            [[ -n "$VENDOR_DIR" ]] || { echo "no vendor/manifest.tsv found"; return 1; }
            git_fetch_pin "$TP/$item" "$(url_of_vendor "$src")" "$(lock_of "$src")" ;;
        git) git_fetch_pin "$TP/$item" "$src" "$chk" ;;
        jar)
            local f="$TP/java/$src"; mkdir -p "$(dirname "$f")"
            curl -sSfL -o "$f.part" "https://repo1.maven.org/maven2/$src" || return 1
            [[ "$(sha_of "$f.part")" == "$chk" ]] || { echo "sha256 mismatch for $src"; rm -f "$f.part"; return 1; }
            mv "$f.part" "$f" ;;
        tarball)
            local f="$TP/$(basename "$src")"; mkdir -p "$TP"
            curl -sSfL -o "$f.part" "$src" || return 1
            [[ "$(sha_of "$f.part")" == "$chk" ]] || { echo "sha256 mismatch for $src"; rm -f "$f.part"; return 1; }
            mv "$f.part" "$f" && tar -xzf "$f" -C "$TP" ;;
    esac
}

# ---- FFTW, built (unit 11) ----------------------------------------------------
# The fftw pin above is the source tarball; the math/ fft competitor links the
# single-precision library built from it, user-level, into third_party/fftw-install.
# Cached: the stamp records version, tarball sha256 and configure flags, so a
# re-run with the same pin and flags is a check, not a rebuild; configure is
# skipped when the build directory's config.status was made with the same flags.
# Measured on Phoenix 2026-10-25: 102 s for configure + make -j32 + install (logs/timing.tsv).
FFTW_FLAGS="--enable-float --enable-sse2 --enable-avx --enable-avx2 --enable-fma --disable-fortran --disable-shared --enable-static"
fftw_stamp() { printf '%s %s %s\n' "$(pin_field fftw 5)" "$(pin_field fftw 6)" "$FFTW_FLAGS"; }
check_fftw_build() {
    local inst="$TP/fftw-install"
    [[ -f "$inst/lib/libfftw3f.a" && -f "$inst/include/fftw3.h" && -f "$inst/.stamp" ]] || return 1
    [[ "$(cat "$inst/.stamp")" == "$(fftw_stamp)" ]] || return 1
    out "$(pin_field fftw 5) (float; built)" "$inst" "libfftw3f.a + stamp == pins.tsv fftw ($FFTW_FLAGS)"
}
install_fftw_build() {
    local src="$TP/fftw-$(pin_field fftw 5)" bld="$TP/fftw-build" inst="$TP/fftw-install"
    [[ -d "$src" ]] || install_pin fftw || return 1
    mkdir -p "$bld"
    if ! [[ -f "$bld/config.status" && -f "$bld/.flags" && "$(cat "$bld/.flags")" == "$FFTW_FLAGS" ]]; then
        rm -rf "$bld" && mkdir -p "$bld" || return 1
        ( cd "$bld" && "$src/configure" --prefix="$inst" $FFTW_FLAGS CFLAGS="-O3" ) || return 1
        printf '%s' "$FFTW_FLAGS" > "$bld/.flags"
    fi
    make -C "$bld" -j"$(nproc)" && make -C "$bld" install && fftw_stamp > "$inst/.stamp"
}
prereq_fftw_build() {
    command -v make >/dev/null && command -v cc >/dev/null && return 0
    echo "apt install make gcc (to build FFTW)"; return 1
}

# ---- the item registry ---------------------------------------------------------
# name|kind|check|install|prereq   (install/prereq may be "-"; check rc 2 = n/a)
REGISTRY=(
    "g++|toolchain|check_gxx|-|prereq_gxx"
    "clang-fork|toolchain|check_clang_fork|-|-"
    "cmake|toolchain|check_cmake|-|prereq_cmake"
    "rustc|toolchain|check_rustc|install_rust|-"
    "cargo|toolchain|check_cargo|install_rust|-"
    "go|toolchain|check_go|-|prereq_go"
    "java|toolchain|check_java|-|-"
    "javac|toolchain|check_javac|-|-"
    "maven|toolchain|check_maven|-|prereq_maven"
    "node|toolchain|check_node|-|-"
    "python3|toolchain|check_python3|-|prereq_python3"
    "dotnet|toolchain|check_dotnet|install_dotnet|prereq_dotnet"
    "cuda-nvcc|toolchain|check_cuda|-|-"
    "rocm-hipcc|toolchain|check_rocm|-|-"
    "oha|toolchain|check_oha|install_oha|-"
    "wrk|toolchain|check_wrk|install_wrk|prereq_wrk"
)
while IFS=$'\t' read -r n k _rest; do
    [[ -z "$n" || "$n" == \#* ]] && continue
    REGISTRY+=("$n|$k|check_pin $n|install_pin $n|-")
done < "$PINS"
REGISTRY+=("fftw-build|library|check_fftw_build|install_fftw_build|prereq_fftw_build")
REGISTRY+=("python-venv|toolchain|check_venv|install_venv|prereq_python3")
while read -r line; do
    d="${line%%[=<> ]*}"; [[ -z "$d" || "$d" == \#* ]] && continue
    REGISTRY+=("py-$d|library|check_pypkg $d|install_pypkgs|prereq_python3")
done < "$REQS"
REGISTRY+=("llama.cpp|checkout|check_llama|-|-")
REGISTRY+=("py-torch|library|check_torch|install_torch|prereq_python3")   # last: the big download

# ---- entry points -------------------------------------------------------------
case "${1:-}" in
    --list-items) for r in "${REGISTRY[@]}"; do echo "${r%%|*}"; done; exit 0 ;;
    __step) shift; mkdir -p "$TP"; eval "$*"; exit $? ;;   # one install step, run under timeout
    --verify) MODE=verify ;;
    "") MODE=install ;;
    *) sed -n '2,30p' "$SELF"; exit 2 ;;
esac

mkdir -p "$TP" "$LOGS"
rows=(); missing=(); needs_sudo=(); timing=()
injected() { [[ ",${CAJETA_SETUP_INJECT_MISSING:-}," == *",$1,"* ]]; }
record() { rows+=("$(printf '%s\t%s\t%s' "$1" "$2" "$3")"); }   # item kind out-line

run_step() {  # name install-command
    local name="$1" cmd="$2" log="$LOGS/$1.log" t0=$SECONDS rc
    printf '[step]  %-28s cap %ss, log %s\n' "$name" "$STEP_CAP" "${log#$REPO/}"
    { echo "# $(date -Is) $cmd"; timeout -k 30 "$STEP_CAP" "$SELF" __step "$cmd"; } >"$log" 2>&1
    rc=$?
    timing+=("$(printf '%s\t%s\t%s' "$name" "$((SECONDS - t0))" "$rc")")
    (( rc == 124 )) && echo "        $name: timed out after ${STEP_CAP}s (see log)"
    (( rc != 0 && rc != 124 )) && echo "        $name: install failed rc=$rc (see log)"
    return $rc
}

T_ALL=$SECONDS
echo "competitors/setup.sh: machine=$MACHINE mode=$MODE tsv=${TSV#$REPO/}"
[[ -n "$VENDOR_DIR" ]] || echo "warn: no vendor/manifest.tsv found; vendorlock pins will be missing"
grep -qi microsoft /proc/version 2>/dev/null && echo "note: WSL2 — governor not visible; NVIDIA driver from /usr/lib/wsl/lib"
declare -A installed=()
for r in "${REGISTRY[@]}"; do
    IFS='|' read -r name kind check install prereq <<<"$r"
    if injected "$name"; then
        record "$name" "$kind" "$(out missing - "CAJETA_SETUP_INJECT_MISSING")"; missing+=("$name"); continue
    fi
    if [[ "$name" == py-torch && "${CAJETA_SETUP_SKIP_TORCH:-0}" == 1 ]] && ! line="$($check)"; then
        record "$name" "$kind" "$(out "skipped (CAJETA_SETUP_SKIP_TORCH=1)" - "not installed or checked")"; continue
    fi
    line="$($check)"; rc=$?
    if (( rc == 2 )); then
        record "$name" "$kind" "$(out "n/a (not on this machine)" - "$check found no install")"; continue
    fi
    if (( rc != 0 )) && [[ "$prereq" != - ]] && ! $prereq >/dev/null; then
        hint="$($prereq)"
        record "$name" "$kind" "$(out "missing (needs sudo)" - "$hint")"; needs_sudo+=("$name: $hint"); continue
    fi
    if (( rc != 0 )) && [[ "$MODE" == install && "$install" != - && -z "${installed[$install]:-}" ]]; then
        installed[$install]=1
        step="${install#install_}"; run_step "${step// /-}" "$install" && { line="$($check)"; rc=$?; }
    fi
    if (( rc == 0 )); then
        record "$name" "$kind" "$line"; printf '[ok]    %-28s %s\n' "$name" "$(cut -f1 <<<"$line")"
    else
        record "$name" "$kind" "$(out missing - "$check failed")"; missing+=("$name")
    fi
done

# Jackson cross-check: the jars must match vendor/jackson.lock (the existing pin).
if [[ -n "$VENDOR_DIR" && -f "$VENDOR_DIR/jackson.lock" ]]; then
    while IFS=$'\t' read -r a v; do
        [[ -z "$a" ]] && continue
        p="$(pin_field "$a" 5)"
        [[ "$p" == "$v" ]] || echo "warn: $a is $p in pins.tsv but $v in vendor/jackson.lock"
    done < "$VENDOR_DIR/jackson.lock"
fi

tmp="$(mktemp "${TSV}.XXXXXX")"
{ printf 'item\tkind\tversion\tpath\tverified\n'; printf '%s\n' "${rows[@]}"; } > "$tmp" && mv "$tmp" "$TSV"
echo "wrote ${TSV#$REPO/} (${#rows[@]} items) in $((SECONDS - T_ALL))s"

if (( ${#timing[@]} )); then
    echo "install steps (name, seconds, rc):"; printf '  %s\n' "${timing[@]}"
    printf '%s\n' "${timing[@]}" | sed "s/^/$(date -I)\t$MACHINE\t/" >> "$LOGS/timing.tsv"
fi
if (( ${#needs_sudo[@]} )); then
    echo; echo "needs sudo (not attempted; run these yourself, then re-run setup.sh):"
    printf '  %s\n' "${needs_sudo[@]}"
fi
if (( ${#missing[@]} )); then
    echo
    for m in "${missing[@]}"; do echo "missing pin: $m" >&2; done
    exit 1
fi
exit 0
