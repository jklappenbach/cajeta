#!/usr/bin/env bash
# search competitors: every drivers/search/<program>.sh, as schema-v2 rows with
# catalog ids (search/<slug>). `search.sh <program>` runs one. See run.sh.
exec bash "$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )/run.sh" search "$@"
