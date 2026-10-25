#!/usr/bin/env bash
# concurrent/ competitor smoke (benchmark-reference unit 7, plan 7.1.2). Runs
# competitors/concurrent.sh (programs `concurrent` and `footprint`) under
# CAJETA_BENCH_SMOKE=1 with one warm-up and three trials, and asserts:
#   (1) every row is schema v2 with machine/os/platform, area concurrent, and
#       an id from bench/concurrent/*.md;
#   (2) each of cpp, rust, go, java, python emits every id (ok or skipped);
#       an ok row has check_ok true and a skipped row a reason;
#   (3) the competitors each page names are ok rows (C++ moodycamel, Rust
#       crossbeam + std mpsc + tokio, Go, Java ScopedValue + ThreadLocal + virtual
#       threads, Python asyncio/threading/contextvars); tasks-1m-footprint may
#       instead skip with its MemAvailable reason;
#   (4) anti-elision: a counted ok row's min_ns is at least input_size/20;
#   (5) shrunk rows say "smoke size" in notes; the sleep rows say overshoot;
#       the footprint rows carry peak_rss_kb and their B/task note.
# CAJETA_BENCH_OUT=<dir> puts the rows and stderr there (default /tmp).
set -uo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
PROJ="$( cd -- "${SCRIPT_DIR}/.." &> /dev/null && pwd )"
RUNNER="${PROJ}/competitors/concurrent.sh"
NCOLS="$(awk -F, '{print NF; exit}' "${PROJ}/competitors/columns.txt")"
OUTDIR="${CAJETA_BENCH_OUT:-/tmp}"; mkdir -p "$OUTDIR"

fail() { echo "CONC-COMP FAIL: $1" >&2; exit 1; }

IDS="atomic-fetchadd atomic-fetchadd-32 atomic-cas atomic-contended-16 mutex-uncontended
mutex-contended-4 mutex-contended-16 rwlock-read-heavy lock-uncontended semaphore-acquire-release
channel-spsc channel-mpsc channel-mpmc fiber-sleep-wake fiberlocal-get-set task-spawn-await tasks-1m-footprint"

echo "[conc-comp] building + running concurrency competitors (smoke sizes; compiles cargo/g++/go/javac on first run)..."
t0=$SECONDS
OUT="$(CAJETA_BENCH_SMOKE=1 PROFILE_RUN_ID=conc-comp PROFILE_RUN_TS=0 PROFILE_WARMUP=1 PROFILE_TRIALS=3 \
       bash "$RUNNER" 2>"$OUTDIR/conc-comp.err")" || { tail -5 "$OUTDIR/conc-comp.err"; fail "concurrent.sh exited non-zero"; }
echo "[conc-comp] run $((SECONDS - t0))s"
printf '%s\n' "$OUT" > "$OUTDIR/conc-comp.csv"
[[ -n "$OUT" ]] || fail "concurrent.sh produced no rows"

# (1) + per-row checks
while IFS= read -r row; do
    [[ -z "$row" ]] && continue
    N="$(printf '%s' "$row" | awk -F, '{print NF}')"
    [[ "$N" == "$NCOLS" ]] || fail "row has $N cols, expected $NCOLS: $row"
    [[ "$(printf '%s' "$row" | cut -d, -f1)" == "2" ]] || fail "row is not schema_version 2: $row"
    printf '%s' "$row" | awk -F, '$33=="" || $34=="" || $37=="" || $37!=$10 {exit 1}' \
        || fail "row lacks machine/os/platform (platform must be the language): $row"
    IFS=, read -r -a c <<< "$row"
    bench="${c[3]}"; area="${c[4]}"; input="${c[8]}"; lang="${c[9]}"; mn="${c[16]}"
    rss="${c[22]}"; status="${c[27]}"; chk="${c[28]}"; notes="${c[29]}"
    [[ "$area" == concurrent ]] || fail "unexpected area: $area"
    case " $(echo $IDS) " in *" $bench "*) ;; *) fail "unexpected benchmark: $bench" ;; esac
    if [[ "$status" == ok ]]; then
        [[ "$chk" == true ]] || fail "ok row failed its check: $row"
        [[ "$mn" =~ ^[0-9]+$ ]] || fail "ok row min_ns=$mn: $row"
        [[ "$notes" == *"smoke size"* ]] || fail "$lang $bench does not say it ran at smoke size: $notes"
        case "$bench" in
            fiber-sleep-wake) [[ "$notes" == *overshoot* ]] || fail "$lang sleep row lacks the overshoot note" ;;
            tasks-1m-footprint)
                [[ "$rss" =~ ^[0-9]+$ && "$rss" -gt 0 ]] || fail "$lang footprint has no peak_rss_kb"
                [[ "$notes" == *"B/task"* ]] || fail "$lang footprint lacks the B/task note" ;;
            *) (( mn * 20 >= input )) || fail "$lang $bench min_ns=$mn for $input operations looks elided" ;;
        esac
    elif [[ "$status" == skipped ]]; then
        [[ -n "$notes" ]] || fail "skip row missing reason: $row"
    else
        fail "unexpected status '$status': $row"
    fi
done <<< "$OUT"

has() {  # lang bench status [library]
    echo "$OUT" | awk -F, -v l="$1" -v b="$2" -v s="$3" -v lib="${4:-}" \
        '$10==l && $4==b && $28==s && (lib=="" || $12==lib) {f=1} END {exit !f}'
}

# (2) every language emits every id
for lang in cpp rust go java python; do
    for b in $IDS; do
        echo "$OUT" | awk -F, -v l="$lang" -v b="$b" '$10==l && $4==b {f=1} END {exit !f}' || fail "$lang emitted no $b row"
    done
done

# (3) the page-named competitors are measured
ok_or_mem_skip() {  # lang
    has "$1" tasks-1m-footprint ok && return 0
    echo "$OUT" | awk -F, -v l="$1" '$10==l && $4=="tasks-1m-footprint" && $28=="skipped" && $30 ~ /MemAvailable/ {f=1} END {exit !f}'
}
for b in $IDS; do
    case "$b" in fiber-sleep-wake|tasks-1m-footprint) ;; *) has cpp "$b" ok || fail "no ok cpp $b row" ;; esac
    for lang in rust go java; do
        [[ "$b" == tasks-1m-footprint ]] && continue
        has "$lang" "$b" ok || fail "no ok $lang $b row"
    done
done
for b in atomic-fetchadd atomic-contended-16 mutex-uncontended mutex-contended-4 mutex-contended-16 \
         fiber-sleep-wake fiberlocal-get-set task-spawn-await; do
    has python "$b" ok || fail "no ok python $b row"
done
for lang in rust go java python; do ok_or_mem_skip "$lang" || fail "$lang tasks-1m-footprint neither ok nor skipped for memory"; done
for b in channel-spsc channel-mpsc channel-mpmc; do
    has cpp "$b" ok moodycamel::BlockingConcurrentQueue || fail "no moodycamel $b row"
    has rust "$b" ok crossbeam-channel || fail "no crossbeam $b row"
done
has rust channel-spsc ok std::sync::mpsc::sync_channel || fail "no std mpsc spsc row"
has rust channel-mpsc ok std::sync::mpsc::sync_channel || fail "no std mpsc mpsc row"
has rust semaphore-acquire-release ok tokio::sync::Semaphore || fail "no tokio Semaphore row"
has rust task-spawn-await ok tokio::spawn || fail "no tokio spawn row"
has java fiberlocal-get-set ok ScopedValue || fail "no Java ScopedValue row"
has java fiberlocal-get-set ok ThreadLocal || fail "no Java ThreadLocal row"
has go task-spawn-await ok goroutine || fail "no goroutine spawn row"
has python task-spawn-await ok asyncio || fail "no asyncio spawn row"

NOK="$(echo "$OUT" | awk -F, '$28=="ok"' | wc -l)"; NSK="$(echo "$OUT" | awk -F, '$28=="skipped"' | wc -l)"
echo "[conc-comp] $NOK ok rows, $NSK skipped rows across cpp rust go java python"
echo "CONC-COMP OK"
