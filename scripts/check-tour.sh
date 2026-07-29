#!/usr/bin/env bash
# Tour gate (docs-refactor plan 11.1.1 / 11.3.1).
#
# 1. Every tour/**/*Demo.cajeta is registered in Tour.cajeta.
# 2. The tour builds with zero warnings.
# 3. The tour runs, exits 0, and prints no FAIL: lines (the demos'
#    self-checks all hold — see DemoClass.check()).
# The xpu tour (samples/tour/xpu/) is a separate entry point, not gated here.
set -uo pipefail

REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." &>/dev/null && pwd)"
CAJETA="${CAJETA:-$REPO_ROOT/build/src/cajeta}"
TOUR="$REPO_ROOT/samples/tour"
MAIN="$TOUR/src/main/cajeta/tour/Tour.cajeta"

[ -x "$CAJETA" ] || { echo "compiler not found: $CAJETA" >&2; exit 2; }
fails=0

# 1. Registration completeness.
while IFS= read -r f; do
    demo="$(basename "$f" .cajeta)"
    [ "$demo" = "DemoClass" ] && continue
    if ! grep -q "heap ${demo}()" "$MAIN"; then
        echo "FAIL: $demo not registered in Tour.cajeta"
        fails=$((fails+1))
    fi
done < <(find "$TOUR/src" -name '*Demo.cajeta')

# 2. Build, zero warnings.
BUILDLOG="$(mktemp)"
RUNLOG="$(mktemp)"
trap 'rm -f "$BUILDLOG" "$RUNLOG"' EXIT
(cd "$TOUR" && "$CAJETA" build) >"$BUILDLOG" 2>&1
if [ $? -ne 0 ]; then
    echo "FAIL: tour build failed"; tail -20 "$BUILDLOG"; fails=$((fails+1))
elif grep -qiE 'warning' "$BUILDLOG"; then
    echo "FAIL: tour build has warnings:"
    grep -iE 'warning' "$BUILDLOG" | head -10
    fails=$((fails+1))
else
    echo "ok:   tour builds, zero warnings"
fi

# 3. Run: exit 0, no FAIL: lines, completion banner present.
if [ "$fails" -eq 0 ]; then
    (cd "$TOUR" && ./build/tour) >"$RUNLOG" 2>&1
    rc=$?
    if [ "$rc" -ne 0 ]; then
        echo "FAIL: tour exited $rc"
        grep -E '^  FAIL:|=== tour' "$RUNLOG" | head -10
        fails=$((fails+1))
    elif grep -q '^  FAIL:' "$RUNLOG"; then
        echo "FAIL: self-check failures despite exit 0 (exit-code wiring broken?)"
        grep '^  FAIL:' "$RUNLOG" | head -10
        fails=$((fails+1))
    elif ! grep -q '=== tour complete' "$RUNLOG"; then
        echo "FAIL: completion banner missing"
        fails=$((fails+1))
    else
        echo "ok:   tour runs, exit 0, $(grep -o '[0-9]* self-checks passed' "$RUNLOG")"
    fi
fi

if [ "$fails" -gt 0 ]; then echo "check-tour: $fails failure(s)"; exit 1; fi
echo "check-tour: OK"
