#!/usr/bin/env bash
# aot competitors: every drivers/aot/<program>.sh, as schema-v2 rows with
# catalog ids (aot/<slug>). `aot.sh <program>` runs one. See run.sh.
exec bash "$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )/run.sh" aot "$@"
