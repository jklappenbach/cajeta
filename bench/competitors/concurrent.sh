#!/usr/bin/env bash
# concurrent competitors: every drivers/concurrent/<program>.sh, as schema-v2 rows with
# catalog ids (concurrent/<slug>). `concurrent.sh <program>` runs one. See run.sh.
exec bash "$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )/run.sh" concurrent "$@"
