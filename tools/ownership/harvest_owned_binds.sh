#!/usr/bin/env bash
# harvest_owned_binds.sh — enumerate every plain `=` bind of a `#T` result.
#
# stdlib-ownership-convention plan, 8.2.7 (spec §4.6). Same instrument as
# 3.3.3's captured-borrow harvest, pointed at the other end of the call: a
# warn-mode build reports every site and keeps going, so ONE pass per library
# enumerates the population instead of one ~90s rebuild per site.
#
# Two knobs, both needed:
#   CAJETA_OWNED_BIND=warn        demote the 8.2.7 check to a report
#   CAJETA_AUDIT_RETURN_TITLES=1  also emit the ASSIGNMENT-position sizing
#                                 records (`pos=assign`), which the check does
#                                 NOT reject — 8.2.7 covers declarations, where
#                                 the number is known, and 8.2.12 decides on
#                                 assignments once this harvest has counted
#                                 them.
#
# The clean is not optional: the check runs during CODEGEN, and an incremental
# build that reuses cached bitcode reports nothing — which reads as "this
# library has no sites".
#
# Usage: tools/ownership/harvest_owned_binds.sh [output-dir]
#   CAJETA_HARVEST_LIBS="cajeta-codec ..."   override the library list

set -uo pipefail

here="$(cd "$(dirname "$0")/../.." && pwd)"
out="${1:-$here/build/owned-bind-harvest-2}"
mkdir -p "$out"

export CAJETA_OWNED_BIND=warn
export CAJETA_AUDIT_RETURN_TITLES=1
export CAJETA_SOURCE_ROOT="$here"
CAJETA="${CAJETA:-$here/build/src/cajeta}"

if [ ! -x "$CAJETA" ]; then
    echo "no compiler at $CAJETA — build the cajeta target first" >&2
    exit 1
fi

# `cajeta-llama` excluded for the reason 8.1.1's harvest excluded it: that
# checkout sits on the developer's parked branch and this script cleans first.
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
    printf '%s — %s decl / %s assign\n' "$status" \
        "$(grep -c 'owned-bind.*callee=[^ ]*$' "$out/$lib.stderr" 2>/dev/null || echo 0)" \
        "$(grep -c 'pos=assign' "$out/$lib.stderr" 2>/dev/null || echo 0)"
done

echo
echo "logs in $out; distinct sites:"
# `|| true` because zero is the SUCCESS outcome and grep exits 1 on no match —
# under `pipefail` that made a clean harvest report failure.
cat "$out"/*.stderr 2>/dev/null \
    | { grep -o '\[owned-bind\].*' || true; } | sort -u | wc -l
