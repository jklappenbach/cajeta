#!/usr/bin/env bash
# bench/lang/Dispatch.md competitors. Java's call-template row is its erased generic over Integer (boxed).
# The program exists in cpp, rust, go, java and python under
# competitors/<lang>/lang/dispatch/; lib/lang-programs.sh builds and runs each and
# stands in a skipped row per id for a language that cannot run.
set -uo pipefail
LANG_PROG=dispatch
LANG_CLASS=DispatchBench
LANG_SLUGS="call-direct call-virtual call-interface call-template call-lambda call-method-ref instanceof"
# shellcheck source=../../lib/lang-programs.sh
source "$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )/../../lib" &> /dev/null && pwd )/lang-programs.sh"
lang_run_all 
