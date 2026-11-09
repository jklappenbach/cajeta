#!/usr/bin/env bash
# lang competitors: every drivers/lang/<program>.sh, as schema-v2 rows with
# catalog ids (lang/<slug>). `lang.sh <program>` runs one. See run.sh.
exec bash "$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )/run.sh" lang "$@"
