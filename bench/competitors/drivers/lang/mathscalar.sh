#!/usr/bin/env bash
# bench/lang/Math.md scalar rows (saxpy and dot-product are drivers/lang/math.sh). Python's transcendental row is NumPy.
# The program exists in cpp, rust, go, java and python under
# competitors/<lang>/lang/mathscalar/; lib/lang-programs.sh builds and runs each and
# stands in a skipped row per id for a language that cannot run.
set -uo pipefail
LANG_PROG=mathscalar
LANG_CLASS=MathScalarBench
LANG_SLUGS="math-transcendental math-intdiv math-intrinsics"
# shellcheck source=../../lib/lang-programs.sh
source "$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )/../../lib" &> /dev/null && pwd )/lang-programs.sh"
lang_run_all 
