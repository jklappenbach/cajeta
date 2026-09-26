#!/usr/bin/env bash
# Print cvm's declared version, from tools/cvm/cajeta.json.
#
# cvm's version is its OWN, not the compiler's: a 1.0 cvm installs an 8.0
# cajeta, so the release tag cannot speak for it. The manifest is the source of
# truth and `Cvm.cajeta`'s VERSION constant has to agree, because that is what
# `cvm --version` prints and a package manager reporting one number while the
# binary reports another is a bug report nobody can place.
# scripts/check-cvm-version.sh asserts they agree.
#
# Plain awk, no python: the Windows release runner has no python3, and this runs
# on every leg. cajeta.json is JSONC, so `//` comments outside strings are
# dropped first. The value read is `version` directly inside `details`, and
# anything else (no details object, no version in it, two of them) is an error.
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
MANIFEST="${1:-${ROOT}/tools/cvm/cajeta.json}"

[ -f "$MANIFEST" ] || { echo "error: no manifest at $MANIFEST" >&2; exit 1; }

awk '
{
    line = $0
    n = length(line)
    for (i = 1; i <= n; i++) {
        c = substr(line, i, 1)
        if (in_str) {
            if (esc) { esc = 0; buf = buf c; continue }
            if (c == "\\") { esc = 1; buf = buf c; continue }
            if (c == "\"") {
                in_str = 0
                last_str = buf
                have_str = 1
                continue
            }
            buf = buf c
            continue
        }
        if (c == "/" && substr(line, i + 1, 1) == "/") break
        if (c == "\"") { in_str = 1; buf = ""; continue }
        if (c == ":") {
            if (have_str) { key = last_str; key_depth = depth; want_value = 1 }
            have_str = 0
            continue
        }
        if (c == "{") {
            depth++
            if (want_value && key == "details" && key_depth == 1) details_depth = depth
            want_value = 0
            have_str = 0
            continue
        }
        if (c == "}") {
            if (want_value && have_str && key == "version" && details_depth && key_depth == details_depth) {
                found++; value = last_str
            }
            want_value = 0
            if (depth == details_depth) details_depth = 0
            depth--
            have_str = 0
            continue
        }
        if (c == "," || c == "[" || c == "]") {
            if (want_value && have_str && key == "version" && details_depth && key_depth == details_depth) {
                found++; value = last_str
            }
            want_value = 0
            have_str = 0
            continue
        }
    }
    if (in_str) { print "error: unterminated string on line " NR > "/dev/stderr"; bad = 1; exit }
}
END {
    if (bad) exit 1
    if (want_value && have_str && key == "version" && details_depth && key_depth == details_depth) {
        found++; value = last_str
    }
    if (found == 0) { print "error: tools/cvm/cajeta.json has no details.version" > "/dev/stderr"; exit 1 }
    if (found > 1)  { print "error: tools/cvm/cajeta.json declares details.version " found " times" > "/dev/stderr"; exit 1 }
    if (value == "") { print "error: tools/cvm/cajeta.json has an empty details.version" > "/dev/stderr"; exit 1 }
    print value
}
' "$MANIFEST"
