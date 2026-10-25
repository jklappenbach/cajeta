#!/usr/bin/env bash
# toolchain/ smoke (Cajeta side; benchmark-reference unit 15, spec §6.14).
# Builds the release harness, runs `--area toolchain` under CAJETA_BENCH_SMOKE=1
# (one warm-up and three trials for every row, the compile rows included), and
# asserts every id and variant on bench/toolchain/*.md emits a row in area
# `toolchain`, tagged `smoke` and `external`, with status ok and check_ok true.
# Two rows may instead be `skipped`, each only with its reason:
#   * kernel-startup, when the compiler was built without libzmq ("libzmq")
#     or the venv lacks jupyter_client ("jupyter_client");
#   * compile-lto/thin, when no ld.lld can be found ("lld").
# Anti-elision: each ok row's min_ns must reach a floor the work cannot beat
# (a cold compile is seconds, a process launch is tens of microseconds), the
# compile rows must carry the child's peak RSS, and the binary-size rows'
# memory column (peak_rss_kb = size on disk in KiB) must be plausible.
#
# Expected wall time on Phoenix, loaded: about 70 minutes (the tour's cold
# debug build is 3 to 5 minutes and runs 4 times; the 10k program 1.5 minutes,
# 4 times at debug and 12 times across the LTO variants).
#
# Env: CAJETA (compiler), CAJETA_BENCH_OUT (directory for the CSV and log;
# default /tmp), NO_BUILD=1 (reuse build/bench), SMOKE_BENCH=<name> (run and
# check one benchmark only, for development).
set -euo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
PROJ="$( cd -- "${SCRIPT_DIR}/.." &> /dev/null && pwd )"
REPO_ROOT="$( cd -- "${PROJ}/.." &> /dev/null && pwd )"
CAJETA="${CAJETA:-${REPO_ROOT}/build/src/cajeta}"
export CAJETA
OUTDIR="${CAJETA_BENCH_OUT:-/tmp}"; mkdir -p "$OUTDIR"

fail() { echo "TOOLCHAIN FAIL: $1" >&2; exit 1; }
[[ -x "$CAJETA" ]] || fail "cajeta not found at $CAJETA"

cd "$PROJ"
if [[ "${NO_BUILD:-0}" != 1 ]]; then
    echo "[toolchain] building (release)..."
    t0=$SECONDS
    "$CAJETA" release >"$OUTDIR/toolchain-build.log" 2>&1 || { tail -30 "$OUTDIR/toolchain-build.log"; fail "release build failed"; }
    echo "[toolchain] built in $((SECONDS - t0))s"
fi

OUT="$OUTDIR/profile-toolchain.csv"
LOG="$OUTDIR/toolchain-run.log"
rm -f "$OUT"
SEL=(--area toolchain)
[[ -n "${SMOKE_BENCH:-}" ]] && SEL=(--bench "$SMOKE_BENCH")
t0=$SECONDS
echo "[toolchain] running ${SEL[*]} (smoke) from $(date +%H:%M)..."
CAJETA_BENCH_SMOKE=1 PROFILE_RUN_ID=toolchain-run "$PROJ/build/bench" --run "${SEL[@]}" --out "$OUT" >"$LOG" 2>&1 \
    || { tail -20 "$LOG"; fail "--run exited non-zero (see $LOG)"; }
echo "[toolchain] ran in $((SECONDS - t0))s"
# The harness quotes a field holding a comma (RFC 4180, dev.cajeta.bench
# 0.1.2's installed build already does): read the CSV with a real parser and
# check tab-separated fields.
TSV="$OUT.tsv"
python3 -c 'import csv, sys
for r in csv.reader(open(sys.argv[1], newline="")):
    print("\t".join(f.replace("\t", " ") for f in r))' "$OUT" > "$TSV"

# id  variant  min_ns floor  (variant "-" = the default "")
CHECKS='
compile-tour-cold - 5000000000
compile-tour-warm - 10000000
compile-10k - 5000000000
compile-lto off 5000000000
compile-lto thin 5000000000
compile-lto full 5000000000
startup-exe - 50000
startup-cja-cold - 1000000000
startup-cja-warm - 10000000
binary-size-hello - 50000
binary-size-tour exe 1000000
binary-size-tour cja 1000000
build-noop - 1000000
kernel-startup - 100000000
debugger-attach - 100000000
'
while read -r b v floor; do
    [[ -z "$b" ]] && continue
    [[ -n "${SMOKE_BENCH:-}" && "$b" != "$SMOKE_BENCH" ]] && continue
    [[ "$v" == "-" ]] && v=""
    row="$(awk -F'\t' -v b="$b" -v v="$v" 'NR>1 && $4==b && $31==v {print; exit}' "$TSV")"
    tag="$b${v:+/$v}"
    [[ -n "$row" ]] || fail "no row for toolchain/$tag"
    f() { printf '%s' "$row" | awk -F'\t' -v c="$1" '{print $c}'; }
    [[ "$(f 5)" == "toolchain" ]] || fail "$tag area=$(f 5) (want toolchain)"
    notes="$(f 30)"
    [[ "$notes" == *smoke* ]] || fail "$tag row not tagged smoke"
    st="$(f 28)"
    if [[ "$st" == "skipped" ]]; then
        case "$tag" in
            kernel-startup) [[ "$notes" == *libzmq* || "$notes" == *jupyter_client* ]] || fail "$tag skipped without its reason: $notes" ;;
            compile-lto/thin) [[ "$notes" == *lld* ]] || fail "$tag skipped without its reason: $notes" ;;
            *) fail "$tag skipped: $notes" ;;
        esac
        echo "[toolchain] $tag skipped: $notes"
        continue
    fi
    [[ "$st" == "ok" ]] || fail "$tag status=$st notes=$notes"
    [[ "$(f 29)" == "true" ]] || fail "$tag check_ok not true (notes: $notes)"
    [[ "$notes" == *external* ]] || fail "$tag notes lack 'external'"
    min="$(f 17)"; rss="$(f 23)"
    (( min >= floor )) || fail "$tag min_ns=$min is under the $floor floor (elided or not run)"
    case "$b" in
        compile-*) (( rss > 10000 )) || fail "$tag peak_rss_kb=$rss (the compiler's peak RSS is missing)" ;;
        binary-size-hello) (( rss >= 100 && rss < 1000000 )) || fail "$tag size $rss KiB is implausible" ;;
        binary-size-tour) (( rss >= 500 && rss < 10000000 )) || fail "$tag size $rss KiB is implausible" ;;
    esac
    [[ "$b" != binary-size-* || "$notes" == *"size on disk"* ]] || fail "$tag does not say its memory column is the size on disk"
    echo "[toolchain] $tag ok  min_ns=$min peak_rss_kb=$rss"
done <<< "$CHECKS"

echo "TOOLCHAIN OK"
