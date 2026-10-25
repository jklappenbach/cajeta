#!/usr/bin/env bash
# bench/lang/String.md (the unit-5 rows) and StringBuilder.md competitors.
# The program exists in cpp, rust, go, java and python under
# competitors/<lang>/lang/text/; lib/lang-programs.sh builds and runs each and
# stands in a skipped row per id for a language that cannot run.
set -uo pipefail
LANG_PROG=text
LANG_CLASS=TextBench
LANG_SLUGS="string-codepoint-walk string-byteat-scan string-split string-compare stringbuilder-append stringbuilder-append-mixed stringbuilder-tostring"
# shellcheck source=../../lib/lang-programs.sh
source "$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )/../../lib" &> /dev/null && pwd )/lang-programs.sh"
lang_run_all 
