#!/usr/bin/env bash
# bench/lang/Optional.md, Pair.md, Slice.md and stream/ArrayStream.md competitors.
# The program exists in cpp, rust, go, java and python under
# competitors/<lang>/lang/values/; lib/lang-programs.sh builds and runs each and
# stands in a skipped row per id for a language that cannot run.
set -uo pipefail
LANG_PROG=values
LANG_CLASS=ValuesBench
LANG_SLUGS="optional-map-unwrap pair-construct slice-window slice-iterate arraystream-iterate"
# shellcheck source=../../lib/lang-programs.sh
source "$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )/../../lib" &> /dev/null && pwd )/lang-programs.sh"
lang_run_all 
