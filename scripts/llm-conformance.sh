#!/usr/bin/env bash
# The kernel conformance corpus over cajeta-llm (xpu-kernel-independence
# Unit 1, spec §2.1, §7.1).
#
#   scripts/llm-conformance.sh <llm-checkout> <backend> <out-dir>
#
# 1. Runs cajeta-llm's own suite on <backend> (cpu, nvptx, amdgpu) with
#    CAJETA_XPU_RECORD, so every kernel it launches is recorded with its
#    inputs and the backend's outputs, under <out-dir>/rec.
# 2. Builds the llm library and compiles llm's test binary again with
#    CAJETA_XPU_CONFORMANCE, which replays each recording through the
#    reference interpreter against this compiler's kernels and stops before
#    codegen. The table lands in <out-dir>/rec/conformance.tsv.
#
# The corpus is pinned to the cajeta-llm revision in
# test/conformance/llm-revision; CI checks that out, and a local run against
# another revision is warned about, not refused.
#
# The held list is test/conformance/llm-held.tsv. The exit status is the
# corpus's: 1 when a launch failed, a hold went stale, or the interpreter met
# undefined behaviour; the llm suite's own result is reported, not gated on.
#
# CAJETA (the compiler, default build/src/cajeta) and CUDA_PATH pass through.
set -uo pipefail

if [ $# -ne 3 ]; then
    echo "usage: $0 <llm-checkout> <backend> <out-dir>" >&2
    exit 2
fi
here="$(cd "$(dirname "$0")/.." && pwd)"
llm="$(cd "$1" && pwd)"
backend="$2"
out="$3"
CAJETA="${CAJETA:-$here/build/src/cajeta}"
mkdir -p "$out"
out="$(cd "$out" && pwd)"
rm -rf "$out/rec"

pinned="$(tr -d '[:space:]' < "$here/test/conformance/llm-revision")"
actual="$(git -C "$llm" rev-parse HEAD)"
if [ "$actual" != "$pinned" ]; then
    echo "::warning::cajeta-llm is at ${actual:0:7}; the corpus is pinned to ${pinned:0:7} (test/conformance/llm-revision)"
fi
echo ">> conformance: recording cajeta-llm ${actual:0:7} on $backend"
# CAJETA_XPU_DEFER=0: a deferred launch runs at a later flush and cannot be
# recorded; recording synchronizes around every launch anyway, so nothing is
# timed here and every launch is taken as it comes.
( cd "$llm" && CAJETA="$CAJETA" XPU_BACKEND="$backend" RELEASE_PASS=0 \
      CAJETA_XPU_RECORD="$out/rec" CAJETA_XPU_DEFER=0 \
      CAJETA_XPU_RECORD_MAX_BYTES="${CAJETA_XPU_RECORD_MAX_BYTES:-16777216}" \
      ./run-tests.sh ) > "$out/leg.log" 2>&1
suite_rc=$?
grep -E '^Tests: |^kernel census' "$out/leg.log" | sed 's/^/   /'
echo ">> conformance: the llm suite returned rc $suite_rc"

# The dependency archives the suite resolved, as it printed them.
dep() { sed -n "s/^>> $1: \(.*\.cja\)\$/\1/p" "$out/leg.log" | tail -1; }
unit="$(dep cajeta-unit)"
codec="$(dep dev.cajeta.codec)"
jinja="$(dep dev.cajeta.jinja)"
logging="$(dep dev.cajeta.logging)"
for d in "$unit" "$codec" "$jinja" "$logging"; do
    if [ -z "$d" ] || [ ! -f "$d" ]; then
        echo "::error::a dependency archive the llm suite used was not found in its log" >&2
        exit 1
    fi
done

recorded=$(find "$out/rec" -name launch.json 2>/dev/null | wc -l)
echo ">> conformance: $recorded launches recorded"
if [ -s "$out/rec/skipped.tsv" ]; then
    echo ">> conformance: launches not recorded, by reason"
    cut -f2 "$out/rec/skipped.tsv" | sort | uniq -c | sort -rn | sed 's/^/   /'
fi
if [ "$recorded" -eq 0 ]; then
    echo "::error::nothing was recorded: an empty corpus is not a passing one" >&2
    exit 1
fi

echo ">> conformance: building the llm library"
mkdir -p "$out/lib"
"$CAJETA" --emit=cja -o "$out/lib/llama.cja" --classpath="$codec,$jinja,$logging" \
    dev.cajeta.llm.Llm.run "$llm/src/main/cajeta" "$out/lib" > /dev/null 2> "$out/lib.err" \
    || { cat "$out/lib.err" >&2; exit 1; }

echo ">> conformance: replaying through the reference interpreter"
CAJETA_XPU_CONFORMANCE="$out/rec" \
CAJETA_XPU_CONFORMANCE_HELD="$here/test/conformance/llm-held.tsv" \
"$CAJETA" --emit=exe --profile=test --xpu-backend="$backend" \
    --classpath="$out/lib/llama.cja,$unit,$codec,$jinja,$logging" \
    -o "$out/lib/unused" \
    dev.cajeta.llm.selftest.TestMain.run "$llm/src/test/cajeta" "$out/lib" \
    > /dev/null 2> "$out/replay.err"
corpus_rc=$?
grep '^cajeta: conformance' "$out/replay.err"
if ! grep -q '^cajeta: conformance: [0-9]* launches' "$out/replay.err"; then
    echo "::error::the replay did not run" >&2
    tail -20 "$out/replay.err" >&2
    exit 1
fi
exit $corpus_rc
