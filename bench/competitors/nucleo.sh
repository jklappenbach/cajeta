#!/usr/bin/env bash
# nucleo competitors: every drivers/nucleo/<program>.sh, as schema-v2 rows with
# catalog ids (nucleo/<slug>). `nucleo.sh <program>` runs one. See run.sh.
exec bash "$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )/run.sh" nucleo "$@"
