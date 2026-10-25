#!/usr/bin/env bash
# gfx competitors: every drivers/gfx/<program>.sh, as schema-v2 rows with
# catalog ids (gfx/<slug>). `gfx.sh <program>` runs one. See run.sh.
exec bash "$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )/run.sh" gfx "$@"
