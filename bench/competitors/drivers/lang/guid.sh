#!/usr/bin/env bash
# bench/lang/Guid.md competitors: libuuid, the uuid crate, google/uuid, java.util.UUID, Python uuid.
# The program exists in cpp, rust, go, java and python under
# competitors/<lang>/lang/guid/; lib/lang-programs.sh builds and runs each and
# stands in a skipped row per id for a language that cannot run.
set -uo pipefail
LANG_PROG=guid
LANG_CLASS=GuidBench
LANG_SLUGS="guid-generate guid-parse guid-format"
# shellcheck source=../../lib/lang-programs.sh
source "$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )/../../lib" &> /dev/null && pwd )/lang-programs.sh"
lang_run_all -luuid
