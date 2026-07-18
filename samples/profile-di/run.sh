#!/usr/bin/env bash
# Build + verify the DI profile / @TestComponent demo end to end.
#
# Compiles the SAME source under --profile=prod, dev, test and checks
# that the compile-time DI graph resolves a different Store / Greeting
# each time — profile selection, @Profile({...}) any-of, and a
# @TestComponent masking a profile-neutral @Component by shared interface.
#
# Exits nonzero on any compile failure or unexpected selection, so this
# script is both the demo and its test. Override the compiler via
# CAJETA_BIN=/path/to/cajeta.
set -euo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &>/dev/null && pwd )"
REPO_ROOT="$( cd -- "${SCRIPT_DIR}/../.." &>/dev/null && pwd )"
CAJETA_BIN="${CAJETA_BIN:-${REPO_ROOT}/build/src/cajeta}"
SRC="${SCRIPT_DIR}/src"
BUILD="${SCRIPT_DIR}/build"
ENTRY="profiledi.App.main"

if [[ ! -x "$CAJETA_BIN" ]]; then
    echo "error: cajeta compiler not found at $CAJETA_BIN" >&2
    echo "       build it first: cd $REPO_ROOT && ./build.sh" >&2
    exit 1
fi

rm -rf "$BUILD"
mkdir -p "$BUILD"

declare -A EXPECT=(
    [prod]="RESULT store=sql greeting=real-greeting"
    [dev]="RESULT store=mem greeting=real-greeting"
    [test]="RESULT store=mem greeting=fake-greeting"
)

fail=0
for prof in prod dev test; do
    out="${BUILD}/app-${prof}"
    if ! "$CAJETA_BIN" --emit=exe --profile="$prof" -o "$out" \
            "$ENTRY" "$SRC" "${BUILD}/obj-${prof}" \
            > "${BUILD}/compile-${prof}.log" 2>&1; then
        echo "FAIL [$prof]: compile failed (see ${BUILD}/compile-${prof}.log)"
        tail -5 "${BUILD}/compile-${prof}.log"
        fail=1
        continue
    fi
    got="$("$out" | grep '^RESULT' || true)"
    if [[ "$got" == "${EXPECT[$prof]}" ]]; then
        echo "ok   [$prof]: $got"
    else
        echo "FAIL [$prof]: got '$got' want '${EXPECT[$prof]}'"
        fail=1
    fi
done

if [[ $fail -eq 0 ]]; then
    echo "PASS: profile-di demo"
else
    echo "FAILED: profile-di demo"
    exit 1
fi
