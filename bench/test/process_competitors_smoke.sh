#!/usr/bin/env bash
# process/ competitor smoke (benchmark-reference unit 13, plan 13.1.2): under CAJETA_BENCH_SMOKE=1,
# competitors/run.sh process emits, for every id on the process pages (bench/process/*.md) and every
# language the pages name (cpp rust go java python), a schema-v2 row with the catalog id process/<slug>,
# status ok, check_ok true, a positive min_ns (the timed work ran and was checked, not
# elided) and notes naming the smoke size. A skipped row fails the smoke: these programs
# use standard libraries only, so nothing here may be missing on a set-up machine.
# Env: CAJETA_BENCH_OUT (directory for the rows and stderr, default /tmp).
set -uo pipefail

PKG=process
LANGS="cpp rust go java python"
SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
PROJ="$( cd -- "${SCRIPT_DIR}/.." &> /dev/null && pwd )"
OUTD="${CAJETA_BENCH_OUT:-/tmp}"; mkdir -p "$OUTD"
TAG="$(printf '%s' "$PKG" | tr a-z A-Z)"
fail() { echo "${TAG}-COMP FAIL: $1" >&2; exit 1; }

t0=$SECONDS
OUT="$(cd "${PROJ}/competitors" && CAJETA_BENCH_SMOKE=1 PROFILE_RUN_ID=$PKG-comp PROFILE_RUN_TS=0 PROFILE_TRIALS=3 PROFILE_WARMUP=1 \
        bash run.sh "$PKG" 2>"$OUTD/$PKG-comp.err")" || fail "run.sh $PKG exited non-zero (see $OUTD/$PKG-comp.err)"
echo "[$PKG-comp] run $((SECONDS - t0)) s"
[[ -n "$OUT" ]] || fail "run.sh $PKG produced no rows"
printf '%s\n' "$OUT" > "$OUTD/$PKG-comp.csv"

while IFS= read -r row; do
    [[ -z "$row" ]] && continue
    IFS=, read -r -a c <<< "$row"
    [[ "${#c[@]}" -ge 36 && "${c[0]}" == "2" && "${c[4]}" == "$PKG" ]] || fail "row not v2 $PKG: $row"
    [[ "${c[27]}" == "ok" && "${c[28]}" == "true" ]] || fail "${c[9]} $PKG/${c[3]} status=${c[27]} check=${c[28]} notes=${c[29]}"
    [[ "${c[16]}" =~ ^[0-9]+$ && "${c[16]}" -gt 0 ]] || fail "${c[9]} $PKG/${c[3]} min_ns=${c[16]} (elided?)"
    [[ "${c[29]}" == *smoke* ]] || fail "${c[9]} $PKG/${c[3]} notes do not name the smoke size"
done <<< "$OUT"

ids="$(grep -ho "^| \`$PKG/[a-z0-9-]*\`" "$PROJ"/$PKG/*.md | sed "s/.*$PKG\///; s/\`//" | sort -u)"
[[ -n "$ids" ]] || fail "no ids found on the $PKG pages"
for lang in $LANGS; do
    for id in $ids; do
        echo "$OUT" | awk -F, -v l="$lang" -v b="$id" '$10==l && $4==b' | grep -q . || fail "no $lang $PKG/$id row"
        echo "[$PKG-comp] $lang $PKG/$id ok"
    done
done
echo "${TAG}-COMP OK ($(echo "$ids" | wc -w) ids x $(echo $LANGS | wc -w) languages, $(echo "$OUT" | grep -c .) rows)"
