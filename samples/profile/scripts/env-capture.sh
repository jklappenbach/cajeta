#!/usr/bin/env bash
# Emit the hardware/OS environment block as key,value CSV rows (spec §3.1, §4.1).
# The Cajeta harness writes the run-level env.csv (run id, timestamps it can read
# via System.env); this driver-side helper appends the host block the language
# can't reach without a subprocess — CPU model, core count, kernel, OS, the
# scaling governor (pinning state), and total memory. Append to env.csv:
#   scripts/env-capture.sh >> results/<ts>/env.csv
set -euo pipefail

emit() { printf '%s,%s\n' "$1" "$2"; }

cpu_model="$(awk -F: '/model name/{gsub(/^ +/,"",$2); print $2; exit}' /proc/cpuinfo 2>/dev/null || true)"
kernel="$(uname -sr 2>/dev/null || true)"
os="$(. /etc/os-release 2>/dev/null && echo "${PRETTY_NAME:-}" || true)"
cores="$(nproc 2>/dev/null || true)"
gov="$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo unknown)"
memkb="$(awk '/MemTotal/{print $2; exit}' /proc/meminfo 2>/dev/null || true)"

emit cpu_model "${cpu_model:-unknown}"
emit cpu_cores "${cores:-unknown}"
emit kernel    "${kernel:-unknown}"
emit os        "${os:-unknown}"
emit governor  "${gov:-unknown}"
emit mem_total_kb "${memkb:-unknown}"
