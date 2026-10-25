#!/usr/bin/env bash
# io/ smoke (benchmark-reference unit 10, Cajeta side). Every id on the io
# pages (bench/io/Buffer.md, file/*.md, net/*.md, net/uri/*.md) must emit a
# row under CAJETA_BENCH_SMOKE=1 with status ok and check_ok true, except one
# recorded skip that is a stdlib gap, asserted as such:
#   watcher-event-latency   cajeta.io.file.Watcher is a placeholder (no watch
#                           call, no inotify backing, no event stream).
# A skip that turns into a measured ok row passes and is reported.
# Also asserted: every ok row has a positive median and input size (nothing
# elided) and a check= value in its notes; the file rows name a storage
# device that is not tmpfs and record the smoke size; the net rows name their
# port and process id; the run leaves no listener or bound UDP socket on the io ports
# (61100..61109); and with the data directory on tmpfs the file rows skip
# with the reason instead of measuring RAM.
# Env: CAJETA (compiler), CAJETA_BENCH_IO_DIR (the file rows' directory;
# default $CAJETA_BENCH_OUT/io-data, which must be on a storage device),
# CAJETA_BENCH_OUT (where the CSVs land; default /tmp), NO_BUILD=1 (reuse
# build/bench).
set -euo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
source "$SCRIPT_DIR/csv_lib.sh"   # cawk / csv_fields: CSV-aware column checks
PROJ="$( cd -- "${SCRIPT_DIR}/.." &> /dev/null && pwd )"
REPO_ROOT="$( cd -- "${PROJ}/.." &> /dev/null && pwd )"
CAJETA="${CAJETA:-${REPO_ROOT}/build/src/cajeta}"
OUTDIR="${CAJETA_BENCH_OUT:-/tmp}"
IODIR="${CAJETA_BENCH_IO_DIR:-${OUTDIR}/io-data}"

fail() { echo "IO FAIL: $1" >&2; exit 1; }
[[ -x "$CAJETA" ]] || fail "cajeta not found at $CAJETA"
mkdir -p "$OUTDIR" "$IODIR"
fstype="$(findmnt -n -o FSTYPE -T "$IODIR" 2>/dev/null || stat -f -c %T "$IODIR")"
case "$fstype" in tmpfs|ramfs) fail "CAJETA_BENCH_IO_DIR=$IODIR is on $fstype; the file rows need a storage device" ;; esac
echo "[io] data directory $IODIR on $(findmnt -n -o SOURCE -T "$IODIR" 2>/dev/null) ($fstype)"

cd "$PROJ"
if [[ -n "${NO_BUILD:-}" && -x "$PROJ/build/bench" ]]; then
    echo "[io] reusing build/bench (NO_BUILD)"
else
    echo "[io] building (release)..."
    t0=$SECONDS
    "$CAJETA" release >/dev/null || fail "release build failed"
    echo "[io] build $((SECONDS - t0)) s"
fi

# Listening TCP and bound UDP sockets on the io ports (TIME-WAIT is not a bind).
ports_busy() { ss -Hltnu 2>/dev/null | awk '{print $5}' | grep -cE ':611(0[0-9])$' || true; }
[[ "$(ports_busy)" == "0" ]] || fail "an io port (61100..61109) is already in use: $(ss -Hltnu | grep -E ':611(0[0-9]) ')"

OUT="$OUTDIR/profile-io.csv"
LOG="$OUTDIR/io-run.log"
rm -f "$OUT"
echo "[io] running --area io (smoke)..."
t0=$SECONDS
CAJETA_BENCH_SMOKE=1 PROFILE_RUN_ID=io-run CAJETA_BENCH_IO_DIR="$IODIR" \
    "$PROJ/build/bench" --run --area io --out "$OUT" >"$LOG" 2>&1 &
pid=$!
echo "[io] bench pid $pid"
wait "$pid" || true
echo "[io] run $((SECONDS - t0)) s"
[[ -s "$OUT" ]] || { tail -20 "$LOG"; fail "--run wrote no CSV"; }

# columns: 4 benchmark, 9 input_size, 17 min_ns, 18 median_ns, 23 peak_rss_kb,
# 28 status, 29 check_ok, 30 notes, 31 variant
row() { cawk -v b="$1" 'NR>1 && $4==b {print; exit}' "$OUT"; }
col() { row "$1" | cawk -v c="$2" '{print $c}'; }

declare -A GAP=( ["watcher-event-latency"]="Watcher is a placeholder" )
FILE_IDS="file-read-sequential file-write-sequential file-random-read file-stat filereader-lines filewriter-lines directory-walk"
NET_IDS="tcp-echo-latency tcp-bulk-throughput tcp-accept-rate udp-send-rate server-1k-idle server-echo-throughput"
IDS="buffer-fill buffer-read-primitives buffer-slice $FILE_IDS path-join-normalize watcher-event-latency $NET_IDS bytebuffer-put-get bufferpool-acquire-release uri-parse uri-build"

for b in $IDS; do
    r="$(row "$b")"
    [[ -n "$r" ]] || fail "$b emitted no row"
    st="$(col "$b" 28)"; ck="$(col "$b" 29)"; nt="$(col "$b" 30)"
    md="$(col "$b" 18)"; isz="$(col "$b" 9)"
    if [[ "$st" == "ok" && "$ck" == "true" ]]; then
        [[ "$md" =~ ^[0-9]+$ && "$md" -gt 0 ]] || fail "$b median_ns=$md (elided?)"
        [[ "$isz" =~ ^[0-9]+$ && "$isz" -gt 0 ]] || fail "$b input_size=$isz"
        [[ "$nt" == *"check="* || "$nt" == *"messages="* ]] || fail "$b notes carry no check value: $nt"
        if [[ -n "${GAP[$b]:-}" ]]; then
            echo "[io] $b ok  (known gap CLOSED: update this smoke and the report)"
        else
            echo "[io] $b ok  median_ns=$md  ${nt}"
        fi
    elif [[ -n "${GAP[$b]:-}" && "$st" == "skipped" && "$nt" == *"${GAP[$b]}"* ]]; then
        echo "[io] $b KNOWN GAP (skipped): $nt"
    else
        fail "$b status=$st check_ok=$ck notes=$nt"
    fi
done

# File rows: the device is named, it is not tmpfs, the smoke size is recorded.
for b in $FILE_IDS; do
    nt="$(col "$b" 30)"
    [[ "$nt" == *"device /"* || "$nt" == *"device "*" at /"* ]] || fail "$b notes name no device: $nt"
    [[ "$nt" != *" tmpfs at "* ]] || fail "$b ran on tmpfs: $nt"
    [[ "$nt" == *"(smoke"* ]] || fail "$b notes do not record the smoke size: $nt"
done
# Net rows: port and pid recorded; the 1k-idle row carries memory and CPU.
for b in $NET_IDS; do
    nt="$(col "$b" 30)"
    [[ "$nt" == *"port"*"pid "* ]] || fail "$b notes lack port/pid: $nt"
done
nt="$(col server-1k-idle 30)"
[[ "$nt" == *"rss_hold_kb="* && "$nt" == *"cpu_ms="* && "$nt" == *"check=1000;"* ]] || fail "server-1k-idle notes: $nt"
[[ "$(col server-1k-idle 23)" =~ ^[0-9]+$ ]] || fail "server-1k-idle has no peak_rss_kb"
[[ "$(col tcp-echo-latency 30)" == *"rtt_median_ns="* ]] || fail "tcp-echo-latency has no per-round-trip median"

# Every row carries its package and the smoke tag.
bad="$(cawk 'NR>1 && ($5 != "io" || $30 !~ /smoke/)' "$OUT" | wc -l)"
[[ "$bad" == "0" ]] || fail "$bad io rows without area io or the smoke tag"
n="$(cawk 'NR>1' "$OUT" | wc -l)"
[[ "$n" == "22" ]] || fail "expected 22 io rows, got $n"

# Nothing is left listening on the io ports.
[[ "$(ports_busy)" == "0" ]] || fail "io ports still bound after the run: $(ss -Hltnu | grep -E ':611(0[0-9]) ')"

# On tmpfs the file rows skip with the reason (not measured RAM).
if [[ -d /dev/shm && "$(findmnt -n -o FSTYPE -T /dev/shm 2>/dev/null)" == "tmpfs" ]]; then
    SHM="$(mktemp -d /dev/shm/cajeta-io-smoke.XXXXXX)"
    OUT2="$OUTDIR/profile-io-tmpfs.csv"; rm -f "$OUT2"
    CAJETA_BENCH_SMOKE=1 PROFILE_RUN_ID=io-tmpfs CAJETA_BENCH_IO_DIR="$SHM" \
        "$PROJ/build/bench" --run --area io --bench file-stat --out "$OUT2" >/dev/null 2>&1 || true
    rm -rf "$SHM"
    st="$(cawk 'NR>1 && $4=="file-stat" {print $28; exit}' "$OUT2")"
    nt="$(cawk 'NR>1 && $4=="file-stat" {print $30; exit}' "$OUT2")"
    [[ "$st" == "skipped" && "$nt" == *"tmpfs"* ]] || fail "file-stat on tmpfs should skip naming tmpfs, got status=$st notes=$nt"
    echo "[io] tmpfs data directory: file rows skip ($nt)"
fi

echo "IO OK"
