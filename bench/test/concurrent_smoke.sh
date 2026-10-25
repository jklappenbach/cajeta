#!/usr/bin/env bash
# concurrent/ smoke (Cajeta side; benchmark-reference unit 7, plan 7.1.1).
# Builds the release harness and runs --area concurrent under
# CAJETA_BENCH_SMOKE=1, then asserts every id on bench/concurrent/*.md has a row:
#   * status ok and check_ok true, tagged smoke, area concurrent;
#   * the multi-fiber rows (atomic-contended-16, mutex-contended-*, rwlock,
#     semaphore, channel-*) ran 1e4 operations and say "smoke size" in notes,
#     as do the 1e8 single-fiber rows (shrunk to 1e6);
#   * anti-elision: a counted row's min_ns is at least input_size/20 (no
#     loop of 1e4+ synchronising operations runs at 20 per nanosecond);
#   * fiber-sleep-wake carries its overshoot note and the stall note;
#   * tasks-1m-footprint is ok with its RSS note, or skipped with a reason
#     naming MemAvailable or vm.max_map_count.
# CAJETA_BENCH_OUT=<dir> puts the CSV and log there (default /tmp).
set -euo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
source "$SCRIPT_DIR/csv_lib.sh"   # cawk / csv_fields: CSV-aware column checks
PROJ="$( cd -- "${SCRIPT_DIR}/.." &> /dev/null && pwd )"
REPO_ROOT="$( cd -- "${PROJ}/.." &> /dev/null && pwd )"
CAJETA="${CAJETA:-${REPO_ROOT}/build/src/cajeta}"
OUTDIR="${CAJETA_BENCH_OUT:-/tmp}"
mkdir -p "$OUTDIR"

fail() { echo "CONCURRENT FAIL: $1" >&2; exit 1; }
[[ -x "$CAJETA" ]] || fail "cajeta not found at $CAJETA"

cd "$PROJ"
if [[ "${NO_BUILD:-0}" != 1 ]]; then
    echo "[concurrent] building (release)..."
    t0=$SECONDS
    CAJETA="$CAJETA" "$CAJETA" release >/dev/null || fail "release build failed"
    echo "[concurrent] build $((SECONDS - t0))s"
fi

OUT="$OUTDIR/profile-concurrent.csv"
LOG="$OUTDIR/conc-run.log"
rm -f "$OUT"
t0=$SECONDS
CAJETA_BENCH_SMOKE=1 PROFILE_RUN_ID=conc-run "$PROJ/build/bench" --run --area concurrent --out "$OUT" >"$LOG" 2>&1 \
    || { grep -vE "example:" "$LOG" | tail -15; fail "--run exited non-zero"; }
echo "[concurrent] run $((SECONDS - t0))s"

IDS="atomic-fetchadd atomic-fetchadd-32 atomic-cas atomic-contended-16
     mutex-uncontended mutex-contended-4 mutex-contended-16 rwlock-read-heavy
     lock-uncontended semaphore-acquire-release
     channel-spsc channel-mpsc channel-mpmc
     fiber-sleep-wake fiberlocal-get-set task-spawn-await tasks-1m-footprint"
SHRUNK="atomic-fetchadd atomic-fetchadd-32 atomic-cas atomic-contended-16
        mutex-uncontended mutex-contended-4 mutex-contended-16 rwlock-read-heavy
        lock-uncontended semaphore-acquire-release channel-spsc channel-mpsc
        channel-mpmc fiberlocal-get-set task-spawn-await"

col() { printf '%s' "$1" | cawk -v c="$2" '{print $c}'; }

for b in $IDS; do
    row="$(cawk -v b="$b" 'NR>1 && $4==b {print; exit}' "$OUT")"
    [[ -n "$row" ]] || fail "no row for $b"
    st="$(col "$row" 28)"; ar="$(col "$row" 5)"; ck="$(col "$row" 29)"; notes="$(col "$row" 30)"
    mn="$(col "$row" 17)"; in="$(col "$row" 9)"
    [[ "$ar" == "concurrent" ]] || fail "$b area=$ar (want concurrent; the id is concurrent/$b)"
    [[ "$notes" == *smoke* ]] || fail "$b row not tagged smoke"
    if [[ "$b" == tasks-1m-footprint && "$st" == skipped ]]; then
        [[ "$notes" == *MemAvailable* || "$notes" == *max_map_count* ]] || fail "$b skipped without its reason: $notes"
        echo "[concurrent] $b skipped: $notes"
        continue
    fi
    [[ "$st" == ok && "$ck" == true ]] || fail "$b status=$st check_ok=$ck notes=$notes"
    case " $(echo $SHRUNK) " in
        *" $b "*) [[ "$notes" == *"smoke size"* ]] || fail "$b does not say it ran at smoke size: $notes" ;;
    esac
    case "$b" in
        fiber-sleep-wake)
            [[ "$notes" == *overshoot* && "$notes" == *p95* ]] || fail "$b lacks the overshoot/p95 note: $notes" ;;
        tasks-1m-footprint)
            [[ "$notes" == *"B/task"* ]] || fail "$b lacks the RSS note: $notes"
            [[ "$(col "$row" 23)" -gt 0 ]] || fail "$b has no peak_rss_kb" ;;
        *)
            [[ "$mn" =~ ^[0-9]+$ ]] || fail "$b min_ns=$mn"
            (( mn * 20 >= in )) || fail "$b min_ns=$mn for $in operations looks elided" ;;
    esac
    echo "[concurrent] $b ok  min_ns=$mn  input=$in"
done

echo "CONCURRENT OK"
