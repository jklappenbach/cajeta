#!/usr/bin/env bash
# bench/lang/ smoke, Cajeta side (benchmark-reference plan 5.1.1 and 5.1.3).
#
# (1) Builds the four flavours of the bench project:
#       release               build/bench               (bounds-check off, live set bounded)
#       release-bounds-on     build/bench-bounds-on     (lang/loop-counted-nobounds:bounds-on)
#       release-live-set-on   build/bench-live-set-on   (lang/alloc-live-set-on)
#       release-live-set-off  build/bench-live-set-off  (lang/alloc-live-set-off)
# (2) Anti-elision: compiles bench/lang/LoopKernels.cajeta alone at
#     --release --opt=O3 --cpu=native, dumps the module as it enters codegen
#     (CAJETA_DUMP_CODEGEN_BC; --emit=ir is the IR BEFORE optimization, so a
#     grep of it would prove nothing) and asserts every loop kernel still has a
#     loop back-edge after O3.
# (3) Runs `--area lang` on build/bench under CAJETA_BENCH_SMOKE=1, then the
#     three single-id runs on the other builds, and asserts every id on the
#     lang pages (test/lang_lib.sh) has a row: area lang, tagged smoke,
#     status ok, check_ok true, with the variants the pages call for (bounds
#     on/off, recoverable/unrecoverable) from the build that owns them, and the
#     live-set rows each from its own build only.
#
# Env: CAJETA (compiler), CAJETA_BENCH_OUT (results dir, default
# /tmp/cajeta-bench-lang-smoke), NO_BUILD=1 (reuse the four binaries).
set -euo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
PROJ="$( cd -- "${SCRIPT_DIR}/.." &> /dev/null && pwd )"
REPO_ROOT="$( cd -- "${PROJ}/.." &> /dev/null && pwd )"
CAJETA="${CAJETA:-${REPO_ROOT}/build/src/cajeta}"
OUTD="${CAJETA_BENCH_OUT:-/tmp/cajeta-bench-lang-smoke}"
# shellcheck source=lang_lib.sh
source "$SCRIPT_DIR/lang_lib.sh"

fail() { echo "LANG FAIL: $1" >&2; exit 1; }
[[ -x "$CAJETA" ]] || fail "cajeta not found at $CAJETA"
lang_lld_path "$REPO_ROOT" "$CAJETA"
mkdir -p "$OUTD"
cd "$PROJ"
t0=$SECONDS

# (2) in the background: it only reads the source tree.
IRD="$OUTD/loop-ir"; rm -rf "$IRD"; mkdir -p "$IRD/src/bench/lang" "$IRD/out" "$IRD/dump"
cp "$PROJ/src/main/cajeta/bench/lang/LoopKernels.cajeta" "$IRD/src/bench/lang/"
( cd "$IRD" && CAJETA_DUMP_CODEGEN_BC="$IRD/dump" "$CAJETA" --release --opt=O3 --cpu=native --emit=obj \
      'bench.lang.LoopKernels::main' src out >"$IRD/compile.log" 2>&1 ) &
IR_PID=$!

# (1)
if [[ "${NO_BUILD:-0}" != "1" ]]; then
    for task in release release-bounds-on release-live-set-on release-live-set-off; do
        tb=$SECONDS
        "$CAJETA" "$task" >"$OUTD/build-$task.log" 2>&1 || { tail -20 "$OUTD/build-$task.log"; fail "$task build failed"; }
        grep -q 'needs lld' "$OUTD/build-$task.log" && echo "[lang] warning: $task built without LTO (no lld)"
        echo "[lang] built $task in $((SECONDS - tb))s"
    done
fi
for b in bench bench-bounds-on bench-live-set-on bench-live-set-off; do
    [[ -x "$PROJ/build/$b" ]] || fail "build/$b missing"
done

# (2) check
wait "$IR_PID" || { tail -5 "$IRD/compile.log"; fail "LoopKernels O3 compile failed"; }
LL="$(ls "$IRD"/dump/*LoopKernels*.codegen.ll 2>/dev/null | head -1)"
[[ -n "$LL" ]] || fail "no post-O3 IR dumped for LoopKernels"
python3 - "$LL" <<'PY' || fail "a loop kernel lost its loop at O3 (see above)"
import re, sys
text = open(sys.argv[1]).read()
want = ["countedSum", "indexedSum", "nestedXor", "foreachSum", "rangeSum", "reduceF64", "earlyExit"]
bad = []
for k in want:
    m = re.search(r'^define [^\n]*LoopKernels::' + k + r'\([^\n]*\{\n(.*?)^\}', text, re.S | re.M)
    if not m:
        bad.append(k + ": function not found"); continue
    body = m.group(1)
    order, back = [], False
    for line in body.split("\n"):
        lm = re.match(r'^([\w.\-]+):\s*(?:;\s*preds = (.*))?$', line)
        if lm:
            name = lm.group(1); order.append(name)
            for p in re.findall(r'%([\w.\-]+)', lm.group(2) or ""):
                if p == name or p not in order[:-1]:
                    back = True      # a predecessor at or after this block: a back-edge
    print(f"[lang] O3 IR {k}: {'loop present' if back else 'NO LOOP'} ({len(order)} blocks)")
    if not back:
        bad.append(k)
sys.exit(1 if bad else 0)
PY

# (3)
CSV="$OUTD/cajeta-lang.csv"; rm -f "$OUTD"/cajeta-lang*.csv
tr=$SECONDS
CAJETA_BENCH_SMOKE=1 PROFILE_RUN_ID=lang-smoke "$PROJ/build/bench" --run --area lang --out "$CSV" >"$OUTD/run.log" 2>&1 \
    || { grep -E '^  (failed|invalid)' "$OUTD/run.log" | head; tail -5 "$OUTD/run.log"; fail "--area lang run exited non-zero"; }
echo "[lang] --area lang ran in $((SECONDS - tr))s"
CAJETA_BENCH_SMOKE=1 PROFILE_RUN_ID=lang-smoke "$PROJ/build/bench-bounds-on" --run --bench loop-counted-nobounds \
    --out "$OUTD/cajeta-lang-bounds-on.csv" >"$OUTD/run-bounds-on.log" 2>&1 || fail "bench-bounds-on run failed"
CAJETA_BENCH_SMOKE=1 PROFILE_RUN_ID=lang-smoke "$PROJ/build/bench-live-set-on" --run --bench alloc-live-set-on \
    --out "$OUTD/cajeta-lang-live-set-on.csv" >"$OUTD/run-live-set-on.log" 2>&1 || fail "bench-live-set-on run failed"
CAJETA_BENCH_SMOKE=1 PROFILE_RUN_ID=lang-smoke "$PROJ/build/bench-live-set-off" --run --bench alloc-live-set-off \
    --out "$OUTD/cajeta-lang-live-set-off.csv" >"$OUTD/run-live-set-off.log" 2>&1 || fail "bench-live-set-off run failed"

# The live-set-off build must not name its row -on (nor the reverse).
awk -F, 'NR>1 && $4=="alloc-live-set-on"' "$OUTD/cajeta-lang-live-set-off.csv" | grep -q . && fail "bench-live-set-off reported alloc-live-set-on"
awk -F, 'NR>1 && $4=="alloc-live-set-off"' "$CSV" | grep -q . && fail "build/bench (live set bounded) reported alloc-live-set-off"

ALL="$OUTD/cajeta-lang-all.csv"
{ cat "$CSV"; for f in cajeta-lang-bounds-on cajeta-lang-live-set-on cajeta-lang-live-set-off; do tail -n +2 "$OUTD/$f.csv"; done; } > "$ALL"
n=0
for e in $LANG_IDS_ALL; do
    slug="${e%%:*}"; var=""; [[ "$e" == *:* ]] && var="${e#*:}"
    row="$(awk -F, -v b="$slug" -v v="$var" 'NR>1 && $4==b && $31==v {print; exit}' "$ALL")"
    [[ -n "$row" ]] || fail "no row for lang/$slug${var:+ [$var]}"
    IFS=, read -r -a c <<< "$row"
    [[ "${c[4]}" == "lang" ]] || fail "lang/$slug area=${c[4]}"
    [[ "${c[29]}" == *smoke* ]] || fail "lang/$slug row not tagged smoke"
    [[ "${c[27]}" == "ok" && "${c[28]}" == "true" ]] || fail "lang/$slug${var:+ [$var]} status=${c[27]} check_ok=${c[28]} notes=${c[29]}"
    printf '[lang] ok  lang/%-28s %-14s min_ns=%s\n' "$slug" "${var:--}" "${c[16]}"
    n=$((n + 1))
done
FID="$(lang_fidelity_tool "$REPO_ROOT" "$CAJETA")" || fail "verify-fidelity.py not found (cajeta-bench checkout; or set VERIFY_FIDELITY)"
# Gate: the schema and fidelity checks over the rows unit 5 added. The whole
# file is checked too and reported: its simd gate covers the older saxpy and
# dot-product rows (measured 2026-10-25 above their ceilings on Phoenix, so
# not a unit-5 regression), and a failure there is printed, not fatal.
U5="$OUTD/cajeta-lang-unit5.csv"
{ head -1 "$ALL"; awk -F, -v ids=" $LANG_IDS_UNIT5 " 'NR>1 { if (index(ids, " " $4 " ") || index(ids, " " $4 ":")) print }' "$ALL"; } > "$U5"
python3 "$FID" "$U5" >"$OUTD/fidelity.log" 2>&1 \
    || { tail -5 "$OUTD/fidelity.log"; fail "verify-fidelity.py rejected the unit-5 lang rows"; }
echo "[lang] verify-fidelity.py (unit-5 rows): ok"
if python3 "$FID" "$ALL" >"$OUTD/fidelity-all.log" 2>&1; then
    echo "[lang] verify-fidelity.py (all lang rows): ok"
else
    echo "[lang] verify-fidelity.py (all lang rows): $(grep -c '\[FAIL\]' "$OUTD/fidelity-all.log") failing check(s), reported, not gated:"
    grep '\[FAIL\]' "$OUTD/fidelity-all.log" | sed 's/^/[lang]   /'
fi
echo "LANG OK ($n rows, $((SECONDS - t0))s)"
