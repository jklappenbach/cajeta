#!/usr/bin/env bash
# collection competitors: every drivers/collection/<program>.sh, as schema-v2 rows with
# catalog ids (collection/<slug>). `collection.sh <program>` runs one. See run.sh.
exec bash "$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )/run.sh" collection "$@"
