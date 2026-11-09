#!/usr/bin/env bash
# hash competitors: every drivers/hash/<program>.sh, as schema-v2 rows with
# catalog ids (hash/<slug>). `hash.sh <program>` runs one. See run.sh.
exec bash "$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )/run.sh" hash "$@"
