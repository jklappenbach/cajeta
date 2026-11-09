#!/usr/bin/env bash
# io competitors: every drivers/io/<program>.sh, as schema-v2 rows with
# catalog ids (io/<slug>). `io.sh <program>` runs one. See run.sh.
exec bash "$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )/run.sh" io "$@"
