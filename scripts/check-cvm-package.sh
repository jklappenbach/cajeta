#!/usr/bin/env bash
# Self-test for the cvm native package (cvm-installer plan 1.1.1 to 1.1.4).
#
# Builds the .deb from an already-built cvm and inspects it without installing.
# The properties that matter are all about what a package manager will DO with
# it: one executable file, a version it can order, and a name that does not
# collide with the compiler's package.
#
# Skips loudly where the tools are absent. dpkg-deb lives on Debian hosts only,
# and cvm has to have been built first.
set -uo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)"
ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
PKGSRC="${ROOT}/tools/cvm/packaging"
CVM_BIN="${CVM_BIN:-${ROOT}/tools/cvm/build/cvm}"

for t in cmake cpack dpkg-deb; do
    command -v "$t" >/dev/null 2>&1 || {
        echo ">> check-cvm-package: $t absent — skipped"; exit 0; }
done
[ -f "$CVM_BIN" ] || {
    echo ">> check-cvm-package: no cvm at $CVM_BIN — skipped (build it with 'cajeta release' in tools/cvm)"
    exit 0; }
[ -f "${PKGSRC}/CMakeLists.txt" ] || {
    echo "FAIL: no packaging project at ${PKGSRC}/CMakeLists.txt"; exit 1; }

fails=0
assert() { # <desc> <expected> <actual>
    if [ "$2" != "$3" ]; then
        echo "FAIL: $1 (expected '$2', got '$3')"
        fails=$((fails + 1))
    fi
}

SCRATCH="${TMPDIR:-${ROOT}/tmp}"
mkdir -p "$SCRATCH"
TMP="$(mktemp -d "${SCRATCH}/cvm-package.XXXXXX")" || exit 1
[ -n "$TMP" ] && [ -d "$TMP" ] || { echo "FAIL: no scratch dir"; exit 1; }
trap 'rm -rf "$TMP"' EXIT

VER="9.9.9"
cmake -S "$PKGSRC" -B "$TMP/b" \
      -DCVM_BINARY="$CVM_BIN" -DCVM_PACKAGE_VERSION="$VER" \
      >"$TMP/cmake.log" 2>&1 || {
    echo "FAIL: configure failed"; tail -15 "$TMP/cmake.log" | sed 's/^/    /'; exit 1; }
( cd "$TMP/b" && cpack -G DEB ) >"$TMP/cpack.log" 2>&1 || {
    echo "FAIL: cpack -G DEB failed"; tail -15 "$TMP/cpack.log" | sed 's/^/    /'; exit 1; }

DEB="$(find "$TMP/b" -maxdepth 1 -name '*.deb' -print -quit)"
[ -n "$DEB" ] || { echo "FAIL: cpack produced no .deb"; exit 1; }

# 1.1.1 exactly one file, the executable, with its executable bit set.
payload="$(dpkg-deb -c "$DEB" | awk '$0 !~ /\/$/ {print}')"
assert "1.1.1 the package carries exactly one file" "1" \
    "$(printf '%s\n' "$payload" | grep -c . )"
assert "1.1.1 that file is executable" "yes" \
    "$(printf '%s' "$payload" | awk '{print ($1 ~ /^-rwxr-xr-x$/) ? "yes" : "no"}')"
assert "1.1.1 and it is named cvm" "yes" \
    "$(printf '%s' "$payload" | awk '{print ($NF ~ /\/bin\/cvm$/) ? "yes" : "no"}')"

# 1.1.2 a version a package manager can order.
pkgver="$(dpkg-deb -f "$DEB" Version)"
assert "1.1.2 the declared version is the one asked for" "$VER" "$pkgver"
# And the release packages with cvm's OWN declared version, so a package built
# the way CI builds it carries that rather than the fixture's.
relver="$("${SCRIPT_DIR}/cvm-version.sh")"
assert "1.1.2 cvm declares a version that is not the placeholder" "moved" \
    "$([ "$relver" != "0.1.0" ] && echo moved || echo still-placeholder)"
assert "1.1.2 and it sorts above the previous release" "above" \
    "$(dpkg --compare-versions "$VER" gt "0.29.0" && echo above || echo not-above)"

# 1.1.3 a name of its own, sharing no path with the compiler package.
pkgname="$(dpkg-deb -f "$DEB" Package)"
assert "1.1.3 the package name differs from the compiler's" "differs" \
    "$([ "$pkgname" != "cajeta" ] && echo differs || echo same)"
assert "1.1.3 the name is cajeta-cvm" "cajeta-cvm" "$pkgname"
# The compiler package owns bin/cajeta; this one must not.
assert "1.1.3 it does not ship the compiler binary" "no" \
    "$(dpkg-deb -c "$DEB" | grep -qE '/bin/cajeta$' && echo yes || echo no)"

# 1.1.4 the packaged bytes are the bare asset's bytes.
dpkg-deb -x "$DEB" "$TMP/x"
extracted="$(find "$TMP/x" -type f -name cvm -print -quit)"
assert "1.1.4 the packaged binary is byte-identical to the bare asset" "same" \
    "$([ -n "$extracted" ] && [ "$(sha256sum < "$extracted" | cut -d' ' -f1)" = \
        "$(sha256sum < "$CVM_BIN" | cut -d' ' -f1)" ] && echo same || echo differs)"

if [ "$fails" -eq 0 ]; then
    echo "check-cvm-package: OK ($(basename "$DEB"))"
    exit 0
fi
echo "check-cvm-package: $fails assertion(s) failed"
exit 1
