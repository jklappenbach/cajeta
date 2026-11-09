#!/usr/bin/env bash
# wire competitors: every drivers/wire/<program>.sh, as schema-v2 rows with
# catalog ids (wire/<slug>). `wire.sh <program>` runs one. See run.sh.
exec bash "$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )/run.sh" wire "$@"
