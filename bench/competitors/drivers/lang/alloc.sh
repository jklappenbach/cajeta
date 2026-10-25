#!/usr/bin/env bash
# bench/lang/Allocation.md competitors. alloc-live-set-on/-off are a Cajeta compiler mode, so every language reports them skipped.
# The program exists in cpp, rust, go, java and python under
# competitors/<lang>/lang/alloc/; lib/lang-programs.sh builds and runs each and
# stands in a skipped row per id for a language that cannot run.
set -uo pipefail
LANG_PROG=alloc
LANG_CLASS=AllocBench
LANG_SLUGS="alloc-small-object alloc-churn-1 alloc-churn-10 alloc-transfer-chain alloc-live-set-on alloc-live-set-off alloc-array alloc-struct-value"
# shellcheck source=../../lib/lang-programs.sh
source "$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )/../../lib" &> /dev/null && pwd )/lang-programs.sh"
lang_run_all 
