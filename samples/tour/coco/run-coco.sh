#!/usr/bin/env bash
# Build + measure the Cajeta coverage tour, then read the findings back out.
#
# `cajeta cover` does the work (see cajeta.json). Everything after it here is
# presentation: the artifacts under build/coco are the actual deliverable, and
# this script just walks them so the tour reads as a tour in a terminal.
#
# Both halves are build-tool tasks: `cover` measures, `mutate` mutates what
# `cover` measured. Neither needs a cajeta-coco checkout.
set -uo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
REPO_ROOT="$( cd -- "${SCRIPT_DIR}/../../.." &> /dev/null && pwd )"
CAJETA="${CAJETA:-${REPO_ROOT}/build/src/cajeta}"
OUT="build/coco"

if [[ ! -x "$CAJETA" ]]; then
    echo "error: cajeta compiler not found at $CAJETA" >&2
    echo "       build it first: cd $REPO_ROOT && ./build.sh" >&2
    echo "       or override with CAJETA=/path/to/cajeta" >&2
    exit 1
fi

cd "$SCRIPT_DIR"

echo "=============================================================="
echo " coco tour — instrument, run, report"
echo "=============================================================="
echo
echo "Two front-end passes, ~80s each. The gate is deliberately set"
echo "below this project's real number: the tour is SUPPOSED to have"
echo "gaps, so 'cover' reports them rather than failing."
echo
# The report action gates on `min` and returns non-zero below it. That is the
# correct behavior for a real project and merely noise here, so the exit code
# is captured rather than allowed to abort the script.
"$CAJETA" cover
COVER_EXIT=$?

if [[ ! -f "$OUT/sites.tsv" ]]; then
    echo
    echo "error: no artifacts at $OUT — the instrument action did not complete." >&2
    exit 1
fi

section() { echo; echo "-- $1 ------------------------------------------------"; }

section "1. Where the numbers came from"
printf '  %-22s %s\n' "site table"    "$OUT/sites.tsv ($(wc -l < "$OUT/sites.tsv") probes)"
for f in run/coco.merged.profile run/coco.profile attribution.tsv crap.tsv \
         xref.json coverage.html annotated.html lcov.info coverage.sarif; do
    [[ -f "$OUT/$f" ]] && printf '  %-22s %s\n' "$(basename "$f")" "$OUT/$f"
done

section "2. Risk queue — worst first (crap.tsv)"
if [[ -f "$OUT/crap.tsv" ]]; then
    # coco-crap v1, after the header line:
    #   method \t complexity \t coverage-PER-MILLE \t score-in-TENTHS
    # Both integers on purpose — these files are diffed in CI and float
    # formatting drift would read as a phantom change. Scale on display.
    tail -n +2 "$OUT/crap.tsv" | head -8 | awk -F'\t' '{
        name = $1; sub(/\(.*/, "", name)
        printf "  %-44s complexity %-3s %5.1f%% covered -> CRAP %.1f%s\n",
               name, $2, $3/10, $4/10, ($4 >= 300 ? "   << high risk" : "")
    }'
    echo "  (the threshold is 30)"
else
    echo "  crap.tsv absent — the report action did not run."
fi

section "3. Per-test attribution (attribution.tsv)"
if [[ -f "$OUT/attribution.tsv" ]]; then
    head -12 "$OUT/attribution.tsv" | sed 's/^/  /'
    echo "  A test with no uniquely-covered line is a redundancy candidate:"
    echo "  CouponTests has two, and only the first buys anything."
else
    echo "  attribution.tsv absent — no recognised test runner was on the"
    echo "  classpath, so per-test data was not collected. That is reported"
    echo "  as 'not collected', never as an empty result."
fi

section "4. Mutation — execution is not verification"
# A build-tool action now, so no cajeta-coco checkout is needed. It runs over
# the completed run above: same site table, same profile, same link line.
"$CAJETA" mutate 2>&1 | sed 's/^/  /'

section "5. Read the rest in the IDE"
cat <<'EOF'
  Open this directory in IntelliJ IDEA with the Cajeta plugin, then:
    Run with Coverage      → gutters, highlighting, per-directory rollups
    View > Tool Windows > Cajeta Coverage
      Dead Code  LegacyPricing = DELETION CANDIDATE
                 Coupon.isExpired = NEEDS A TEST
      Tests      CouponTests' second test: no unique coverage
      Risk       TaxTable.rateBasisPoints at the top
      Mutants    Shipping.rateCents, SURVIVED
EOF

echo
echo "coverage gate exit: $COVER_EXIT (non-zero just means below 'min')"
