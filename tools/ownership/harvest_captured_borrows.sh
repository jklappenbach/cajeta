#!/usr/bin/env bash
# harvest_captured_borrows.sh — enumerate every CAPTURED_BORROW_PARAM site.
#
# stdlib-ownership-convention plan, 3.3.3. Unit 3's check landed error-first
# and the gate found 76 failures against an audit that had classified 10. A
# throw stops the build at the FIRST capture, so the remainder cost one ~90s
# compile each and the total was never visible. This runs the check in WARN
# mode (`CAJETA_CAPTURED_BORROW=warn`), where a build reports every site and
# keeps going — one pass per library, whole list.
#
# Shares harvest_return_titles.sh's shape deliberately: same pinning, same
# clean-first rule, same "a failed build still contributes what it compiled".
# The clean is not optional — the check runs during CODEGEN, and an
# incremental build that reuses cached bitcode reports nothing, which reads
# as "this library has no captures".
#
# Usage: tools/ownership/harvest_captured_borrows.sh [output-dir]
#   CAJETA_HARVEST_LIBS="cajeta-codec ..."   override the library list

set -uo pipefail

here="$(cd "$(dirname "$0")/../.." && pwd)"
out="${1:-$here/build/captured-borrow-harvest}"
mkdir -p "$out"

export CAJETA_CAPTURED_BORROW=warn
export CAJETA_SOURCE_ROOT="$here"
CAJETA="${CAJETA:-$here/build/src/cajeta}"

if [ ! -x "$CAJETA" ]; then
    echo "no compiler at $CAJETA — build the cajeta target first" >&2
    exit 1
fi

# `cajeta-llama` is excluded for the same reason 8.1.1's harvest excluded it:
# that checkout sits on the developer's parked branch and this script cleans
# before it builds. Pass it via CAJETA_HARVEST_LIBS to include it.
libs="${CAJETA_HARVEST_LIBS:-cajeta-codec cajeta-logging cajeta-http cajeta-ml cajeta-timeseries}"

for lib in $libs; do
    dir="$here/../$lib"
    if [ ! -d "$dir" ]; then
        echo "skip   $lib — no checkout at $dir"
        continue
    fi
    printf 'build  %s ... ' "$lib"
    ( cd "$dir" && "$CAJETA" clean ) > /dev/null 2>&1 || true
    if ( cd "$dir" && "$CAJETA" build ) \
            > "$out/$lib.stdout" 2> "$out/$lib.stderr"; then
        status=ok
    else
        status="FAILED (partial harvest kept)"
    fi
    printf '%s — %s captured-borrow notes\n' "$status" \
        "$(grep -c 'captured-borrow' "$out/$lib.stderr" 2>/dev/null || echo 0)"
done

echo
echo "logs in $out; distinct sites:"
# `|| true` because zero sites is the SUCCESS outcome and grep exits 1 on no
# match — under `pipefail` that made a clean harvest report failure.
cat "$out"/*.stderr 2>/dev/null \
    | { grep -o '\[captured-borrow\].*' || true; } | sort -u | wc -l
