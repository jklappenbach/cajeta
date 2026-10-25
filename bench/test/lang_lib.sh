#!/usr/bin/env bash
# The ids of bench/lang/ (every page's Benchmarks table), shared by
# lang_smoke.sh and lang_competitors_smoke.sh. One "slug[:variant]" per entry.

LANG_IDS_LOOPS="loop-counted loop-counted-nobounds:bounds-off loop-counted-nobounds:bounds-on loop-nested loop-foreach-array loop-foreach-arraylist loop-range loop-reduce loop-early-exit"
LANG_IDS_EXCEPTIONS="try-no-throw throw-catch-1 throw-catch-10 throw-catch-100 throw-recoverable:recoverable throw-recoverable:unrecoverable throw-in-fiber"
LANG_IDS_ALLOC="alloc-small-object alloc-churn-1 alloc-churn-10 alloc-transfer-chain alloc-live-set-on alloc-live-set-off alloc-array alloc-struct-value"
LANG_IDS_DISPATCH="call-direct call-virtual call-interface call-template call-lambda call-method-ref instanceof"
LANG_IDS_STRING="string-build-concat string-search string-replace string-uppercase string-codepoint-walk string-byteat-scan string-split string-compare"
LANG_IDS_SB="stringbuilder-append stringbuilder-append-mixed stringbuilder-tostring"
LANG_IDS_VALUES="optional-map-unwrap pair-construct slice-window slice-iterate"
LANG_IDS_MATH="math-transcendental math-intdiv math-intrinsics saxpy dot-product"
LANG_IDS_GUID="guid-generate guid-parse guid-format"
LANG_IDS_STREAM="stream-filter-map-reduce stream-parallel-reduce arraystream-iterate"
LANG_IDS_PROGRAMS="clbg-mandelbrot clbg-fannkuch-redux clbg-spectral-norm"

LANG_IDS_ALL="$LANG_IDS_LOOPS $LANG_IDS_EXCEPTIONS $LANG_IDS_ALLOC $LANG_IDS_DISPATCH $LANG_IDS_STRING $LANG_IDS_SB $LANG_IDS_VALUES $LANG_IDS_MATH $LANG_IDS_GUID $LANG_IDS_STREAM $LANG_IDS_PROGRAMS"

# The rows unit 5 added (the competitor programs of drivers/lang/{loops,
# exceptions,alloc,dispatch,text,values,mathscalar,guid}.sh). The eleven older
# lang rows have their own smokes (string, stream, math, clbg).
LANG_IDS_UNIT5="$LANG_IDS_LOOPS $LANG_IDS_EXCEPTIONS $LANG_IDS_ALLOC $LANG_IDS_DISPATCH string-codepoint-walk string-byteat-scan string-split string-compare $LANG_IDS_SB $LANG_IDS_VALUES math-transcendental math-intdiv math-intrinsics $LANG_IDS_GUID arraystream-iterate"

# Put the cajeta-llvm fork's ld.lld on PATH when none is there: the release
# flavor links with --lto=thin, which the compiler drops (loudly) without lld.
# The fork is the compiler repo's sibling: next to the repo the scripts run
# from, or next to the repo the compiler binary ($2) was built in.
lang_lld_path() {
    local repo="$1" cajeta="${2:-}" d base
    command -v ld.lld >/dev/null 2>&1 && return 0
    base=""; [[ -n "$cajeta" ]] && base="$(cd "$(dirname "$cajeta")/../.." 2>/dev/null && pwd)"
    for d in "$repo/../cajeta-llvm/llvm-install/bin" "$repo/../cajeta-llvm/build/bin" \
             ${base:+"$base/../cajeta-llvm/llvm-install/bin"}; do
        if [[ -x "$d/ld.lld" ]]; then
            PATH="$(cd "$d" && pwd):$PATH"; export PATH
            return 0
        fi
    done
    return 0
}

# dev.cajeta.bench's fidelity gate: the cajeta-bench checkout beside the repo.
lang_fidelity_tool() {
    local repo="$1" cajeta="${2:-}" base f
    base=""; [[ -n "$cajeta" ]] && base="$(cd "$(dirname "$cajeta")/../.." 2>/dev/null && pwd)"
    for f in "${VERIFY_FIDELITY:-}" "$repo/../cajeta-bench/tools/verify-fidelity.py" \
             ${base:+"$base/../cajeta-bench/tools/verify-fidelity.py"}; do
        [[ -n "$f" && -f "$f" ]] && { printf '%s' "$f"; return 0; }
    done
    return 1
}
