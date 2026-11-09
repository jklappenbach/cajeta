#!/usr/bin/env bash
# reflect competitors: every drivers/reflect/<program>.sh, as schema-v2 rows with
# catalog ids (reflect/<slug>). `reflect.sh <program>` runs one. See run.sh.
exec bash "$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )/run.sh" reflect "$@"
