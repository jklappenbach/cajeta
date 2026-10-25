#!/usr/bin/env bash
# io/ competitor smoke (benchmark-reference unit 10). Runs every io program
# (competitors/io.sh: drivers/io/io.sh) under CAJETA_BENCH_SMOKE=1 and asserts:
#  - every row is schema v2 (37 columns), area io, platform = language;
#  - every row is ok with check_ok true, or skipped with a reason; nothing is
#    invalid, and an ok row has a positive median (nothing elided);
#  - each competitor the pages name emits each of its ids ok:
#      cpp         Buffer, File, FileInfo, FileReader, FileWriter, Path,
#                  Directory, ByteBuffer, TcpStream, TcpListener, UdpSocket,
#                  Uri, UriBuilder rows
#      rust (std)  the same plus Watcher (notify) and BufferPool (bytes)
#      rust tokio  TcpStream, TcpListener and both Server rows
#      go, java    every id (Server rows: goroutines / a Selector)
#      python      every id but ByteBuffer and BufferPool (the pages name no
#                  Python library for them)
#  - every check=<value> of an id is the same in every language, and equal
#    to the Cajeta row's when bench/test/io_smoke.sh left its CSV in
#    CAJETA_BENCH_OUT: the programs compute the same thing;
#  - server-1k-idle rows carry rss_hold_kb, cpu_ms and a peak_rss_kb;
#  - the run leaves no listener or bound UDP socket on the competitor ports
#    (61200..61799);
#  - with the data directory on tmpfs the file rows are skips naming it.
# Env: CAJETA_BENCH_IO_DIR (default $CAJETA_BENCH_OUT/io-data), CAJETA_BENCH_OUT
# (where the rows and stderr land; default /tmp).
set -uo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
PROJ="$( cd -- "${SCRIPT_DIR}/.." &> /dev/null && pwd )"
OUTDIR="${CAJETA_BENCH_OUT:-/tmp}"
IODIR="${CAJETA_BENCH_IO_DIR:-${OUTDIR}/io-data}"
NCOLS="$(awk -F, '{print NF; exit}' "${PROJ}/competitors/columns.txt")"
mkdir -p "$OUTDIR" "$IODIR"

fail() { echo "IO-COMP FAIL: $1" >&2; exit 1; }
busy() { ss -Hltnu 2>/dev/null | awk '{print $5}' | grep -cE ':61[2-7][0-9][0-9]$' || true; }
[[ "$(busy)" == "0" ]] || fail "a competitor io port (61200..61799) is already in use"

OUT="$OUTDIR/io-comp.csv"; ERR="$OUTDIR/io-comp.err"
echo "[io-comp] building + running the io programs in every language (smoke)..."
t0=$SECONDS
PROFILE_RUN_ID=io-comp PROFILE_RUN_TS=0 CAJETA_BENCH_SMOKE=1 CAJETA_BENCH_IO_DIR="$IODIR" \
    bash "$PROJ/competitors/io.sh" >"$OUT" 2>"$ERR" || fail "io.sh exited non-zero (see $ERR)"
echo "[io-comp] ran in $((SECONDS - t0)) s, $(wc -l < "$OUT") rows"
grep 'pid [0-9]*:' "$ERR" | sed 's/^/[io-comp]   /'
[[ -s "$OUT" ]] || fail "io.sh produced no rows"
grep -q 'rows_v2: dropped' "$ERR" && fail "rows_v2 dropped a malformed line (see $ERR)"

# (1) shape, status, reasons, no elision.
awk -F, -v n="$NCOLS" '
    NF != n                         { print "cols " NF ": " $0; bad = 1; next }
    $1 != "2" || $5 != "io"         { print "schema/area: " $0; bad = 1 }
    $33 == "" || $34 == "" || $37 != $10 { print "machine/os/platform: " $0; bad = 1 }
    $28 == "ok" && ($29 != "true" || $18 !~ /^[0-9]+$/ || $18 + 0 <= 0) { print "ok row: " $0; bad = 1 }
    $28 == "skipped" && $30 == ""   { print "skip without reason: " $0; bad = 1 }
    $28 != "ok" && $28 != "skipped" { print "status " $28 ": " $0; bad = 1 }
    END { exit bad }' "$OUT" || fail "malformed or failing rows above"

# (2) every named competitor, each id ok.
BUF="buffer-fill buffer-read-primitives buffer-slice"
FILE="file-read-sequential file-write-sequential file-random-read file-stat filereader-lines filewriter-lines directory-walk path-join-normalize"
NET="tcp-echo-latency tcp-bulk-throughput tcp-accept-rate udp-send-rate"
SRV="server-echo-throughput server-1k-idle"
URI="uri-parse uri-build"
has_ok() {   # lang lib-test(eq|ne) lib ids...
    local lang="$1" op="$2" lib="$3" b; shift 3
    for b in "$@"; do
        awk -F, -v l="$lang" -v op="$op" -v lib="$lib" -v b="$b" \
            '$10==l && ((op=="eq" && $12==lib) || (op=="ne" && $12!=lib)) && $4==b && $28=="ok" && $29=="true" {f=1} END {exit !f}' "$OUT" \
            || fail "$lang${lib:+ ($op $lib)}: no ok row for io/$b"
    done
    echo "[io-comp] $lang${lib:+ ($op $lib)}: $# ids ok"
}
has_ok cpp ne "" $BUF $FILE bytebuffer-put-get $NET $URI
has_ok rust ne tokio $BUF $FILE watcher-event-latency bytebuffer-put-get bufferpool-acquire-release $NET $URI
has_ok rust eq tokio tcp-echo-latency tcp-bulk-throughput tcp-accept-rate $SRV
has_ok go ne "" $BUF $FILE watcher-event-latency bytebuffer-put-get bufferpool-acquire-release $NET $SRV $URI
has_ok java ne "" $BUF $FILE watcher-event-latency bytebuffer-put-get bufferpool-acquire-release $NET $SRV $URI
has_ok python ne "" $BUF $FILE watcher-event-latency $NET $SRV $URI

# (3) one check value per id, across every language (and the Cajeta row).
checks() { awk -F, '$28=="ok" { if (match($30, /check=[^;]*/)) print $4 "\t" $10 "/" $12 "\t" substr($30, RSTART + 6, RLENGTH - 6) }' "$1"; }
checks "$OUT" > "$OUTDIR/io-comp.checks"
CAJ="$OUTDIR/profile-io.csv"
if [[ -s "$CAJ" ]]; then
    awk -F, 'NR>1 && $28=="ok" { if (match($30, /check=[^;"]*/)) print $4 "\tcajeta\t" substr($30, RSTART + 6, RLENGTH - 6) }' "$CAJ" >> "$OUTDIR/io-comp.checks"
    echo "[io-comp] comparing against the Cajeta rows in $CAJ"
fi
awk -F'\t' '{ if (!($1 in v)) { v[$1] = $3; who[$1] = $2 } else if (v[$1] != $3) { print "io/" $1 ": " who[$1] " check=" v[$1] " but " $2 " check=" $3; bad = 1 } }
            END { exit bad }' "$OUTDIR/io-comp.checks" || fail "check values disagree across languages"
echo "[io-comp] $(cut -f1 "$OUTDIR/io-comp.checks" | sort -u | wc -l) ids, one check value each across $(cut -f2 "$OUTDIR/io-comp.checks" | sort -u | wc -l) language/library rows"

# (4) the 1k-idle rows carry memory and CPU.
awk -F, '$4=="server-1k-idle" && $28=="ok" && ($30 !~ /rss_hold_kb=[0-9]+/ || $30 !~ /cpu_ms=[0-9]+/ || $23 !~ /^[0-9]+$/) {print; bad=1} END {exit bad}' "$OUT" \
    || fail "a server-1k-idle row lacks rss_hold_kb / cpu_ms / peak_rss_kb"

# (5) nothing left bound.
[[ "$(busy)" == "0" ]] || fail "competitor io ports still bound after the run"

# (6) on tmpfs the file rows skip with the reason (cpp, file rows only).
if [[ "$(findmnt -n -o FSTYPE -T /dev/shm 2>/dev/null)" == "tmpfs" ]]; then
    SHM="$(mktemp -d /dev/shm/cajeta-io-comp.XXXXXX)"
    OUT2="$OUTDIR/io-comp-tmpfs.csv"
    PROFILE_LANGS=cpp PROFILE_IO_IDS="file-stat directory-walk" CAJETA_BENCH_SMOKE=1 CAJETA_BENCH_IO_DIR="$SHM" \
        bash "$PROJ/competitors/io.sh" >"$OUT2" 2>/dev/null
    rm -rf "$SHM"
    for b in file-stat directory-walk; do
        awk -F, -v b="$b" '$4==b && $28=="skipped" && $30 ~ /tmpfs/ {f=1} END {exit !f}' "$OUT2" \
            || fail "cpp $b on tmpfs should be a skip naming tmpfs: $(grep ",$b," "$OUT2")"
    done
    echo "[io-comp] tmpfs data directory: file rows skip"
fi

echo "[io-comp] $(awk -F, '$28=="ok"' "$OUT" | wc -l) ok rows, $(awk -F, '$28=="skipped"' "$OUT" | wc -l) skips with reasons"
echo "IO-COMP OK"
