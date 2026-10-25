#!/usr/bin/env bash
# math competitors: every drivers/math/<program>.sh, as schema-v2 rows with
# catalog ids (math/<slug>). `math.sh <program>` runs one. See run.sh.
exec bash "$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )/run.sh" math "$@"
