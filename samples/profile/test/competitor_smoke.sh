#!/usr/bin/env bash
# Unit 4 TDD smoke test — written before the competitor scaffolds.
# (1) the toolchain probe detects present languages + records absent ones (nvcc)
# as skips; (2) each present competitor language builds + runs a hello-metric
# runner that emits ONE row matching the canonical 31-column schema, with the
# right language id, a positive min_ns, and status ok. A missing toolchain is a
# recorded skip, not a failure.
set -uo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
PROJ="$( cd -- "${SCRIPT_DIR}/.." &> /dev/null && pwd )"
HELLO="${PROJ}/competitors/hello.sh"
PROBE="${PROJ}/scripts/probe-toolchains.sh"
NCOLS="$(awk -F, '{print NF; exit}' "${PROJ}/competitors/columns.txt")"

fail() { echo "COMPETITOR FAIL: $1" >&2; exit 1; }

# (1) probe
PROBEOUT="$(bash "$PROBE")"
echo "[competitor] probe:"; printf '%s\n' "$PROBEOUT" | sed 's/^/  /'
echo "$PROBEOUT" | grep -q "^rust,true,"      || fail "probe didn't detect rust"
echo "$PROBEOUT" | grep -qE "^gpu_cuda,(true|false)," || fail "probe missing gpu_cuda row"

# (2) each present language emits a conformant row
RAN=0
for lang in rust cpp java python go; do
    case "$lang" in
        rust)   command -v cargo   >/dev/null 2>&1 || { echo "[competitor] skip rust (no cargo)";   continue; } ;;
        cpp)    command -v g++     >/dev/null 2>&1 || { echo "[competitor] skip cpp (no g++)";       continue; } ;;
        java)   command -v javac   >/dev/null 2>&1 || { echo "[competitor] skip java (no javac)";    continue; } ;;
        python) command -v python3 >/dev/null 2>&1 || { echo "[competitor] skip python (no python3)";continue; } ;;
        go)     command -v go      >/dev/null 2>&1 || { echo "[competitor] skip go (no go)";         continue; } ;;
    esac
    ROW="$(PROFILE_RUN_ID=comp-run bash "$HELLO" "$lang")" || fail "$lang hello runner failed"
    echo "[$lang] $ROW"
    N="$(printf '%s' "$ROW" | awk -F, '{print NF}')"
    [[ "$N" == "$NCOLS" ]] || fail "$lang row has $N cols, expected $NCOLS"
    [[ "$(printf '%s' "$ROW" | awk -F, '{print $10}')" == "$lang" ]] || fail "$lang language col wrong"
    MIN="$(printf '%s' "$ROW" | awk -F, '{print $17}')"
    [[ "$MIN" =~ ^[0-9]+$ && "$MIN" -gt 0 ]] || fail "$lang min_ns not positive: $MIN"
    [[ "$(printf '%s' "$ROW" | awk -F, '{print $28}')" == "ok" ]] || fail "$lang status not ok"
    RAN=$((RAN + 1))
done

[[ "$RAN" -ge 1 ]] || fail "no competitor language ran"
echo "[competitor] $RAN language(s) emitted conformant rows"

# (3) vendoring infra: --list works and names key suites (no cloning here —
# suites are vendored on demand per area).
VLIST="$(bash "${PROJ}/competitors/vendor/vendor.sh" --list)" || fail "vendor --list failed"
for suite in simdjson highway smhasher stringzilla babelstream; do
    echo "$VLIST" | grep -q "$suite" || fail "vendor manifest missing $suite"
done
echo "[competitor] vendor manifest lists $(echo "$VLIST" | grep -c '\[') suites"

echo "COMPETITOR OK"
