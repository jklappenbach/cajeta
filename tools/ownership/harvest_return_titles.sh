#!/usr/bin/env bash
# harvest_return_titles.sh — collect the Unit 8 ride-through measurement.
#
# stdlib-ownership-convention plan, 8.1.1. Builds each first-party library
# against THIS checkout's compiler and stdlib with the return-title audit on,
# leaving one stderr log per library for report_return_titles.py.
#
# Coverage is what these builds compile. That is the honest limit and the
# report states it: a stdlib method no build reaches is a method nobody
# measured. The union across libraries is what makes it broad — each pulls in a
# different slice of `cajeta.*`.
#
# The same pinning Unit 7's sweep used: CAJETA is this build's binary,
# CAJETA_SOURCE_ROOT is this checkout, so the stdlib measured is the one on
# this branch and not an installed one.
#
# Usage: tools/ownership/harvest_return_titles.sh [output-dir]
#   CAJETA_HARVEST_LIBS="cajeta-codec ..."   override the library list

set -uo pipefail

here="$(cd "$(dirname "$0")/../.." && pwd)"
out="${1:-$here/build/return-title-harvest}"
mkdir -p "$out"

export CAJETA_AUDIT_RETURN_TITLES=1
export CAJETA_SOURCE_ROOT="$here"
CAJETA="${CAJETA:-$here/build/src/cajeta}"

if [ ! -x "$CAJETA" ]; then
    echo "no compiler at $CAJETA — build the cajeta target first" >&2
    exit 1
fi

# `cajeta-llama` is NOT in the default list: that checkout sits on the
# developer's parked `cajeta-llama/unit-13-chat` branch, and this script cleans
# before it builds. Unit 7's sweep left that checkout alone for the same
# reason. Pass it explicitly via CAJETA_HARVEST_LIBS to include it.
libs="${CAJETA_HARVEST_LIBS:-cajeta-codec cajeta-logging cajeta-http cajeta-ml cajeta-timeseries}"

for lib in $libs; do
    dir="$here/../$lib"
    if [ ! -d "$dir" ]; then
        echo "skip   $lib — no checkout at $dir"
        continue
    fi
    printf 'build  %s ... ' "$lib"
    # CLEAN first, always. The audit reports from CODEGEN, and an incremental
    # build that reuses cached bitcode codegens nothing — the first harvest run
    # here produced a 0-byte log for an already-built library and would have
    # been read as "this library has no ride-through sites".
    ( cd "$dir" && "$CAJETA" clean ) > /dev/null 2>&1 || true
    if ( cd "$dir" && "$CAJETA" build ) \
            > "$out/$lib.stdout" 2> "$out/$lib.stderr"; then
        status=ok
    else
        # A library that fails to build still contributes everything it
        # compiled before it stopped; the count is reported either way rather
        # than the log being discarded as unusable.
        status="FAILED (partial harvest kept)"
    fi
    printf '%s — %s title / %s plain\n' "$status" \
        "$(grep -c 'return-title' "$out/$lib.stderr" 2>/dev/null || echo 0)" \
        "$(grep -c 'return-plain' "$out/$lib.stderr" 2>/dev/null || echo 0)"
done

echo
echo "logs in $out; report with:"
echo "  tools/ownership/report_return_titles.py $out/*.stderr > docs/stdlib/return-title-audit.md"
