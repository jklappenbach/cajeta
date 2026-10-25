#!/usr/bin/env bash
# bench/lang/Exceptions.md competitors. Rust propagates a Result with ? and Go returns an error through the same depth (each row's notes say so); Java has a row with and one without a stack trace.
# The program exists in cpp, rust, go, java and python under
# competitors/<lang>/lang/exceptions/; lib/lang-programs.sh builds and runs each and
# stands in a skipped row per id for a language that cannot run.
set -uo pipefail
LANG_PROG=exceptions
LANG_CLASS=ExceptionsBench
LANG_SLUGS="try-no-throw throw-catch-1 throw-catch-10 throw-catch-100 throw-recoverable throw-in-fiber"
# shellcheck source=../../lib/lang-programs.sh
source "$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )/../../lib" &> /dev/null && pwd )/lang-programs.sh"
lang_run_all 
