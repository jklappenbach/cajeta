#!/usr/bin/env bash
# xpu competitors: every drivers/xpu/<program>.sh, as schema-v2 rows with
# catalog ids (xpu/<slug>). `xpu.sh <program>` runs one. See run.sh.
exec bash "$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )/run.sh" xpu "$@"
