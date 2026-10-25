#!/usr/bin/env bash
# Detect competitor toolchains + versions (spec §2.1.4, §16). Emits one
# `lang,present,version` CSV row per language so the driver knows which to run
# and which to record as skips. Absent toolchains are reported present=false,
# never an error — the matrix degrades gracefully.
set -uo pipefail

emit() { printf '%s,%s,%s\n' "$1" "$2" "$3"; }
ver()  { local b="$1"; shift; command -v "$b" >/dev/null 2>&1 && "$@" 2>&1 | head -1 | tr ',' ' ' || echo ""; }

echo "lang,present,version"

if command -v cargo  >/dev/null 2>&1; then emit rust   true  "$(ver rustc rustc --version)"; else emit rust   false ""; fi
if command -v g++    >/dev/null 2>&1; then emit cpp    true  "$(ver g++ g++ --version)";     else emit cpp    false ""; fi
if command -v javac  >/dev/null 2>&1; then emit java   true  "$(ver java java -version)";    else emit java   false ""; fi
if command -v python3>/dev/null 2>&1; then emit python true  "$(ver python3 python3 --version)"; else emit python false ""; fi
if command -v go     >/dev/null 2>&1; then emit go     true  "$(ver go go version)";         else emit go     false ""; fi

# GPU backends for the XPU area (Unit 15).
if command -v nvcc       >/dev/null 2>&1; then emit gpu_cuda   true  "$(ver nvcc nvcc --version)";   else emit gpu_cuda   false ""; fi
if command -v hipcc      >/dev/null 2>&1; then emit gpu_hip    true  "$(ver hipcc hipcc --version)"; else emit gpu_hip    false ""; fi
if command -v vulkaninfo >/dev/null 2>&1; then emit gpu_vulkan true  "present";                      else emit gpu_vulkan false ""; fi
