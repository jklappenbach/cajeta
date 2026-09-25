#!/usr/bin/env bash
# cvm's version is declared in two places and they must agree
# (cvm-installer plan 0.1.2, spec 3.3).
#
#   tools/cvm/cajeta.json  details.version   -> what the native packages declare
#   Cvm.cajeta             VERSION           -> what `cvm --version` prints
#
# Nothing joins them, so a bump can land in one and not the other. A package
# manager reporting one number while the binary reports another is a bug report
# nobody can place, which is the whole reason this check exists.
#
# Also asserts the built binary actually prints it, when one is present. The
# constant agreeing with the manifest is necessary and not sufficient: a stale
# build prints the old number regardless of what the source says.
set -uo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)"
ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
SRC="${ROOT}/tools/cvm/src/main/cajeta/cvm/Cvm.cajeta"

fails=0
assert() { # <desc> <expected> <actual>
    if [ "$2" != "$3" ]; then
        echo "FAIL: $1 (expected '$2', got '$3')"
        fails=$((fails + 1))
    fi
}

declared="$("${SCRIPT_DIR}/cvm-version.sh")" || {
    echo "FAIL: could not read cvm's declared version"; exit 1; }

constant="$(sed -n 's/.*private static String VERSION = "\([^"]*\)".*/\1/p' "$SRC" | head -1)"
assert "the VERSION constant matches the manifest" "$declared" "$constant"

# A monotonic version is what lets a package manager offer an upgrade. 0.1.0 was
# the placeholder that shipped unchanged across every release, so it is
# specifically disallowed rather than merely discouraged.
assert "the declared version is not the 0.1.0 placeholder" "moved" \
    "$([ "$declared" != "0.1.0" ] && echo moved || echo still-placeholder)"

CVM_BIN="${CVM_BIN:-${ROOT}/tools/cvm/build/cvm}"
if [ -x "$CVM_BIN" ]; then
    printed="$("$CVM_BIN" --version 2>/dev/null | awk '{print $2; exit}')"
    assert "the built cvm prints the declared version" "$declared" "$printed"
else
    echo ">> check-cvm-version: no built cvm at $CVM_BIN — skipped the runtime half"
fi

if [ "$fails" -eq 0 ]; then
    echo "check-cvm-version: OK ($declared)"
    exit 0
fi
echo "check-cvm-version: $fails assertion(s) failed"
exit 1
