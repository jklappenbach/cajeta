#!/usr/bin/env bash
# verify-cvm-package.sh [--require] <bare-cvm> <package>...
#
# Asserts each cvm native package (.deb, .rpm, .pkg, .msi) carries exactly one
# entry, the cvm executable, byte-identical to the bare release asset
# (cvm-installer spec 4.4, plan 2.1.1). A package whose format has no unpacking
# tool on this host is skipped, or fails under --require.
set -uo pipefail

require=0
if [ "${1:-}" = "--require" ]; then require=1; shift; fi
[ $# -ge 2 ] || { echo "usage: $0 [--require] <bare-cvm> <package>..." >&2; exit 2; }
BARE="$1"; shift
[ -f "$BARE" ] || { echo "FAIL: no bare cvm at $BARE"; exit 1; }

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)"
SCRATCH="${TMPDIR:-${SCRIPT_DIR}/../tmp}"
mkdir -p "$SCRATCH"
WORK="$(mktemp -d "${SCRATCH}/verify-cvm-package.XXXXXX")" || exit 1
[ -n "$WORK" ] && [ -d "$WORK" ] || { echo "FAIL: no scratch dir"; exit 1; }
trap 'rm -rf "$WORK"' EXIT

have() { command -v "$1" >/dev/null 2>&1; }
sha() { if have sha256sum; then sha256sum < "$1"; else shasum -a 256 < "$1"; fi | cut -d' ' -f1; }
want="$(sha "$BARE")"

# Puts the payload of <pkg> under <dir>, using <dir>.x as scratch; returns 3
# when this host has no tool for the format.
unpack() {
    local pkg="$1" dir="$2" x="$2.x"
    mkdir -p "$x"
    case "$pkg" in
        *.deb)
            if have dpkg-deb; then dpkg-deb -x "$pkg" "$dir"
            elif have 7z; then
                (cd "$x" && 7z x -y "$pkg" >/dev/null) && 7z x -so "$x"/data.tar* | tar -x -C "$dir"
            else return 3; fi ;;
        *.rpm)
            have cpio || return 3
            if have rpm2cpio; then (cd "$dir" && rpm2cpio "$pkg" | cpio -idm --quiet)
            elif have 7z; then (cd "$dir" && 7z x -so "$pkg" | cpio -idm --quiet)
            else return 3; fi ;;
        *.pkg)
            if have pkgutil; then
                rmdir "$x" && pkgutil --expand-full "$pkg" "$x" || return 1
                for p in "$x"/*.pkg/Payload; do cp -Rp "$p"/. "$dir"/ || return 1; done
            elif have 7z && have cpio; then
                (cd "$x" && 7z x -y "$pkg" >/dev/null) || return 1
                for p in "$x"/*.pkg/Payload; do
                    (cd "$dir" && gzip -dc "$p" | cpio -idm --quiet) || return 1
                done
            else return 3; fi ;;
        *.msi)
            if have 7z; then (cd "$dir" && 7z x -y "$pkg" >/dev/null)
            elif have powershell; then
                # An administrative install lays the files out and copies the
                # .msi beside them; msiexec ships with every Windows host.
                local wp wd
                wp="$(cygpath -w "$pkg" 2>/dev/null || printf '%s' "$pkg")"
                wd="$(cygpath -w "$dir" 2>/dev/null || printf '%s' "$dir")"
                MSYS2_ARG_CONV_EXCL='*' powershell -NoProfile -Command "
                  \$p = Start-Process msiexec.exe -Wait -PassThru -ArgumentList @('/a', '\"${wp}\"', '/qn', 'TARGETDIR=\"${wd}\"')
                  exit \$p.ExitCode" || return 1
                find "$dir" -maxdepth 1 -name '*.msi' -delete
            else return 3; fi ;;
        *) echo "FAIL: $(basename "$pkg"): not a .deb, .rpm, .pkg or .msi"; return 1 ;;
    esac
}

fails=0; checked=0
for pkg in "$@"; do
    name="$(basename "$pkg")"
    case "$pkg" in /*) ;; *) pkg="$PWD/$pkg" ;; esac
    dir="$WORK/$name.d"; mkdir -p "$dir"
    unpack "$pkg" "$dir"; rc=$?
    if [ "$rc" -eq 3 ]; then
        if [ "$require" -eq 1 ]; then
            echo "FAIL: $name: no tool on this host can unpack it"; fails=$((fails + 1))
        else
            echo ">> verify-cvm-package: $name: no unpacking tool here, skipped"
        fi
        continue
    fi
    [ "$rc" -eq 0 ] || { echo "FAIL: $name: could not be unpacked"; fails=$((fails + 1)); continue; }
    checked=$((checked + 1))
    entries="$(cd "$dir" && find . ! -type d | sort)"
    count="$(printf '%s\n' "$entries" | grep -c .)"
    if [ "$count" -ne 1 ]; then
        echo "FAIL: $name carries $count entries, want exactly the cvm executable:"
        printf '%s\n' "$entries" | sed 's/^/    /'
        fails=$((fails + 1)); continue
    fi
    f="$dir/${entries#./}"
    case "$f" in
        */cvm|*/cvm.exe|*cvm.exe) ;;
        *) echo "FAIL: $name: the one entry is ${entries#./}, not cvm"; fails=$((fails + 1)); continue ;;
    esac
    [ -L "$f" ] && { echo "FAIL: $name: cvm is a symlink"; fails=$((fails + 1)); continue; }
    if [ "${name##*.}" != "msi" ] && [ ! -x "$f" ]; then
        echo "FAIL: $name: cvm is not executable"; fails=$((fails + 1)); continue
    fi
    if [ "$(sha "$f")" != "$want" ]; then
        echo "FAIL: $name: the packaged cvm differs from the bare asset $(basename "$BARE")"
        fails=$((fails + 1)); continue
    fi
    echo "ok: $name carries exactly cvm, identical to $(basename "$BARE")"
done

[ "$fails" -eq 0 ] || { echo "verify-cvm-package: $fails package(s) failed"; exit 1; }
echo "verify-cvm-package: OK ($checked checked)"
