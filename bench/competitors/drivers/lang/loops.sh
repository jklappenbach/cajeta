#!/usr/bin/env bash
# bench/lang/Loops.md competitors: counted, bounds on/off, nested, foreach over an array and a list, range, float64 reduction, early exit.
# The program exists in cpp, rust, go, java and python under
# competitors/<lang>/lang/loops/; lib/lang-programs.sh builds and runs each and
# stands in a skipped row per id for a language that cannot run.
set -uo pipefail
LANG_PROG=loops
LANG_CLASS=LoopsBench
LANG_SLUGS="loop-counted loop-counted-nobounds:bounds-on loop-counted-nobounds:bounds-off loop-nested loop-foreach-array loop-foreach-arraylist loop-range loop-reduce loop-early-exit"
# shellcheck source=../../lib/lang-programs.sh
source "$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )/../../lib" &> /dev/null && pwd )/lang-programs.sh"
lang_run_all 
