#!/usr/bin/env bash
# bench.sh publish <machine> <run-id> (spec §9.4, plan 22.1.1): collect the
# compiler repo's rows and each ecosystem repo's rows for one run into
# bench/results/<machine>/<run-id>/, gate them, and name the run in LATEST.
# The work is scripts/publish.py; this wrapper only resolves the defaults.
#
# Sources:
#   the compiler repo   BENCH_PUBLISH_SRC (default bench/build/results/<run-id>,
#                       where bench.sh writes a run), results.csv + env.csv
#   each ecosystem repo <repo>/bench/results/<machine>/<run-id>/ for the repos in
#                       BENCH_PUBLISH_REPOS ("name=path ..."; default the
#                       siblings cajeta-llm, cajeta-ml, cajeta-http,
#                       cajeta-primavera, cajeta-codec and ../ml/cajeta-xgboost)
# Destination: BENCH_PUBLISH_DEST (default bench/results).
set -euo pipefail
HERE="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
BENCH="$( cd -- "$HERE/.." &> /dev/null && pwd )"
REPO="$( cd -- "$BENCH/.." &> /dev/null && pwd )"
CODE="$( cd -- "$REPO/../.." &> /dev/null && pwd )"
if [[ $# -ne 2 ]]; then
    echo "usage: bench.sh publish <machine> <run-id>" >&2; exit 2
fi
DEFAULT_REPOS="cajeta-llm=$CODE/cpp/cajeta-llm cajeta-ml=$CODE/cpp/cajeta-ml cajeta-xgboost=$CODE/ml/cajeta-xgboost cajeta-http=$CODE/cpp/cajeta-http cajeta-primavera=$CODE/cpp/cajeta-primavera cajeta-codec=$CODE/cpp/cajeta-codec"
exec python3 "$HERE/publish.py" \
    --machine "$1" --run-id "$2" \
    --src "${BENCH_PUBLISH_SRC:-$BENCH/build/results/$2}" \
    --repos "${BENCH_PUBLISH_REPOS:-$DEFAULT_REPOS}" \
    --dest "${BENCH_PUBLISH_DEST:-$BENCH/results}" \
    --bench "$BENCH" --site "$REPO/site"
