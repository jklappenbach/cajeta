#!/usr/bin/env bash
# run.sh <package> [program ...] — the competitor rows for one catalog package.
#
# Runs every drivers/<package>/<program>.sh (or only the named programs), each
# of which builds and runs that program in every present language, and pipes
# their output through lib/rows.sh's rows_v2 so stdout is schema-v2 rows with
# catalog ids (area = <package>, benchmark = slug), never a header.
#
# Adding a competitor program to a package: put the sources under
# competitors/<lang>/<package>/<program>/ and a driver at
# drivers/<package>/<program>.sh that prints one row per measurement (v1 or
# v2, see lib/rows.sh). Nothing else changes.
#
# Env (passed to the drivers): PROFILE_RUN_ID, PROFILE_RUN_TS,
# PROFILE_WARMUP, PROFILE_TRIALS (short mode: PROFILE_TRIALS=3),
# PROFILE_LANGS, PROFILE_DATA_DIR, CAJETA_BENCH_MACHINE.
set -uo pipefail
HERE="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
# shellcheck source=lib/rows.sh
source "$HERE/lib/rows.sh"

PKG="${1:-}"
[[ -n "$PKG" ]] || { echo "usage: run.sh <package> [program ...]" >&2; exit 2; }
shift
D="$HERE/drivers/$PKG"
PROGRAMS=("$@")
if [[ ${#PROGRAMS[@]} -eq 0 && -d "$D" ]]; then
    for f in "$D"/*.sh; do [[ -e "$f" ]] && PROGRAMS+=("$(basename "$f" .sh)"); done
fi

RC=0
for p in ${PROGRAMS[@]+"${PROGRAMS[@]}"}; do
    drv="$D/$p.sh"
    [[ -f "$drv" ]] || { echo "run.sh: no driver $PKG/$p (drivers/$PKG/$p.sh)" >&2; RC=2; continue; }
    bash "$drv" | rows_v2 "$PKG"
    rc=("${PIPESTATUS[@]}")
    [[ "${rc[0]}" == "0" ]] || { echo "run.sh: $PKG/$p driver exited ${rc[0]}" >&2; RC=1; }
done
exit "$RC"
