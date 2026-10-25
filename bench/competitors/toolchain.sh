#!/usr/bin/env bash
# toolchain competitors: every drivers/toolchain/<program>.sh, as schema-v2 rows with
# catalog ids (toolchain/<slug>). `toolchain.sh <program>` runs one. See run.sh.
exec bash "$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )/run.sh" toolchain "$@"
