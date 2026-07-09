#!/bin/bash
# Refresh test/test-durations.seed.tsv — the checked-in per-test timing seed that
# cajeta_tests.sh packs shards with on a cold clone / CI runner (no local
# .test-durations.tsv yet). Without a seed the LPT packer weights every test
# equally and degenerates to count-based packing, which lets a cluster of slow
# suites become the long pole that bounds the whole sweep.
#
# Two sources, in preference order:
#
#   1. .test-durations.tsv — written by cajeta_tests.sh's parallel path. These are
#      gtest's own per-test `(N ms)` numbers, measured INSIDE a batched suite
#      process, so they exclude the once-per-process stdlib prime. This is what
#      the packer actually models. Prefer it.
#
#   2. build/Testing/Temporary/CTestCostData.txt — CTest's accumulated per-test
#      cost (a moving average over runs, in SECONDS). CTest runs one process per
#      test, so each cost INCLUDES the stdlib prime. That over-weights
#      compile-heavy tests relative to their in-batch cost. Still far better than
#      count-based (a constant), and the only source available before a full
#      parallel sweep has ever run.
#
# Usage:
#   scripts/update-test-durations.sh            # auto-pick the best source
#   scripts/update-test-durations.sh --ctest    # force the CTest cost source
#
# Regenerate after a full sweep, or whenever the suite's shape changes enough
# that shard balance drifts. Check the result in.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$SCRIPT_DIR"

SEED="test/test-durations.seed.tsv"
LOCAL=".test-durations.tsv"
COST="build/Testing/Temporary/CTestCostData.txt"

force_ctest=0
[ "${1:-}" = "--ctest" ] && force_ctest=1

tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT

if [ "$force_ctest" = "0" ] && [ -s "$LOCAL" ]; then
    echo ">> Source: $LOCAL (in-process gtest ms — preferred)"
    sort -t$'\t' -k1,1 "$LOCAL" > "$tmp"
elif [ -s "$COST" ]; then
    echo ">> Source: $COST (per-process seconds; includes the stdlib prime)"
    # <name> <numruns> <cost_seconds>  ->  name<TAB>ms   (drop zero-cost entries:
    # never-run or disabled tests, which would otherwise pin a weight of 0)
    awk 'NF>=3 && $3+0 > 0 { ms = int($3*1000 + 0.5); if (ms < 1) ms = 1;
                             printf "%s\t%d\n", $1, ms }' "$COST" \
        | sort -t$'\t' -k1,1 -u > "$tmp"
else
    echo "error: no timing source found." >&2
    echo "  run the parallel suite once (./cajeta_tests.sh) to produce $LOCAL," >&2
    echo "  or a ctest sweep to produce $COST" >&2
    exit 1
fi

n=$(wc -l < "$tmp" | tr -d ' ')
if [ "$n" -lt 100 ]; then
    echo "error: only $n entries — refusing to overwrite the seed with a partial run." >&2
    echo "  (a filtered run records only the tests it ran)" >&2
    exit 1
fi

old=0
[ -s "$SEED" ] && old=$(wc -l < "$SEED" | tr -d ' ')
mv "$tmp" "$SEED"
trap - EXIT
echo ">> Wrote $SEED: $old -> $n entries"
echo ">> Preview the resulting shard balance without running anything:"
echo "     PLAN_ONLY=1 ./cajeta_tests.sh shard=32 | head -1"
