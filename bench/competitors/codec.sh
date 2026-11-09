#!/usr/bin/env bash
# codec competitors: every drivers/codec/<program>.sh, as schema-v2 rows with
# catalog ids (codec/<slug>). `codec.sh <program>` runs one. See run.sh.
exec bash "$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )/run.sh" codec "$@"
