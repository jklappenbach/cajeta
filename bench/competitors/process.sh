#!/usr/bin/env bash
# process competitors: every drivers/process/<program>.sh, as schema-v2 rows with
# catalog ids (process/<slug>). `process.sh <program>` runs one. See run.sh.
exec bash "$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )/run.sh" process "$@"
