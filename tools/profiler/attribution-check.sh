#!/usr/bin/env bash
# cajeta-profiler 6.1.e — does an optimized build still attribute to source?
#
# Builds one three-deep program at -O0 and again at -O3, profiles both, and
# asserts the same source-level nesting comes out of each:
#
#     nest.Nest.main
#       nest.Nest.outer      <- a one-line forwarder; -O3 inlines it away
#         nest.Nest.inner
#
# The point is `outer`. A profiler reading machine frames loses it at -O3,
# because there is no `outer` frame left to read. Cajeta's line-info probes are
# CALLS with side effects, so inlining carries them into the caller rather than
# deleting them — the frame survives the optimizer that erased the function.
#
# Run by hand, not in CI: it needs a built toolchain and trace_processor, which
# is why the cheap trace validation (.github/workflows/trace-validate.yml) uses
# a plain `cc` instead. Kept as a script rather than a paragraph in the plan so
# the claim can be re-checked rather than re-read.
#
#   CAJETA=build/src/cajeta TRACE_PROCESSOR=./trace_processor tools/profiler/attribution-check.sh
set -euo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd -- "${HERE}/../.." && pwd)"
CAJETA="${CAJETA:-${ROOT}/build/src/cajeta}"
TP="${TRACE_PROCESSOR:-trace_processor}"
WORK="${WORK:-$(mktemp -d)}"

[[ -x "$CAJETA" ]] || { echo "error: no cajeta at $CAJETA (build it, or set CAJETA=)" >&2; exit 2; }
command -v "$TP" >/dev/null 2>&1 || [[ -x "$TP" ]] || {
    echo "error: no trace_processor (get it from https://get.perfetto.dev/trace_processor," >&2
    echo "       then set TRACE_PROCESSOR=/path/to/trace_processor)" >&2; exit 2; }

mkdir -p "$WORK/arch"
fail=0

for opt in O0 O3; do
    exe="$WORK/nest-$opt"
    trace="$WORK/nest-$opt.pftrace"
    echo "== building $opt =="
    "$CAJETA" --emit=exe --opt="$opt" -o "$exe" \
        nest.Nest::main "$HERE/attribution/src" "$WORK/arch" >/dev/null

    echo "== profiling $opt =="
    rm -f "$trace"
    CAJETA_PROFILER=1 CAJETA_PROFILER_HZ=2000 CAJETA_PROFILER_OUT="$trace" "$exe" >/dev/null
    [[ -s "$trace" ]] || { echo "FAIL($opt): no trace written — 4.2.d's exit drain did not fire"; fail=1; continue; }

    # depth is Perfetto's own nesting, computed from the slice stack, so this
    # asserts the SHAPE the reader sees rather than the bytes we wrote.
    got=$("$TP" -q /dev/stdin "$trace" 2>/dev/null <<'SQL' | tail -n +2
select s.depth || ' ' || s.name from slice s
where s.name like 'nest.%' order by s.depth;
SQL
)
    want=$'"0 nest.Nest.main"\n"1 nest.Nest.outer"\n"2 nest.Nest.inner"'
    if [[ "$got" == "$want" ]]; then
        echo "PASS($opt): main -> outer -> inner"
    else
        echo "FAIL($opt): expected"; echo "$want"; echo "got"; echo "$got"
        fail=1
    fi
done

exit $fail
