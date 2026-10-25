#!/usr/bin/env bash
# wire/ competitor smoke (benchmark-reference unit 13, plan 13.1.2): under
# CAJETA_BENCH_SMOKE=1, `run.sh wire` (drivers/wire/wire.sh) emits schema-v2
# rows with catalog ids wire/<slug> for every id each language's page row names,
# each with status ok, check_ok true and a positive min_ns:
#   cpp   protobuf, flatbuffers, capnproto: encoder-encode, encoder-decode
#         flatbuffers, capnproto: struct-view-read
#         protobuf (DynamicMessage), capnproto (DynamicStruct): schemaencoder-encode/decode
#         zlib, zstd: compressstream-roundtrip [zlib] [zstd]
#   java  protobuf-java: encoder-*; avro: schemaencoder-*; java.util.zip, zstd-jni: compressstream
#   rust  bincode: encoder-*; flate2, zstd: compressstream
#   go    compress/flate, klauspost/compress/zstd: compressstream
# Also every page id under bench/wire/ has at least one competitor row.
# Env: CAJETA_BENCH_OUT (log directory, default /tmp).
set -uo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
PROJ="$( cd -- "${SCRIPT_DIR}/.." &> /dev/null && pwd )"
OUTD="${CAJETA_BENCH_OUT:-/tmp}"; mkdir -p "$OUTD"
NCOLS="$(awk -F, '{print NF; exit}' "${PROJ}/competitors/columns.txt")"
fail() { echo "WIRE-COMP FAIL: $1" >&2; exit 1; }

echo "[wire-comp] building + running cpp java rust go (smoke sizes)..."
t0=$SECONDS
OUT="$(cd "${PROJ}/competitors" && CAJETA_BENCH_SMOKE=1 PROFILE_RUN_ID=wire-comp PROFILE_RUN_TS=0 PROFILE_TRIALS=3 PROFILE_WARMUP=1 \
       TMPDIR="$OUTD" bash run.sh wire 2>"$OUTD/wire-comp.err")" || fail "run.sh wire exited non-zero (see $OUTD/wire-comp.err)"
echo "[wire-comp] run $((SECONDS - t0)) s"
[[ -n "$OUT" ]] || fail "no rows"

while IFS= read -r row; do
    [[ -z "$row" ]] && continue
    IFS=, read -r -a c <<< "$row"
    [[ "${#c[@]}" -ge 36 && "${c[0]}" == "2" && "${c[4]}" == "wire" ]] || fail "row not v2 wire: $row"
    n="$(awk -F, '{print NF}' <<< "$row")"
    [[ "$n" == "$NCOLS" ]] || fail "row has $n columns, want $NCOLS: $row"
    [[ "${c[27]}" == "ok" && "${c[28]}" == "true" ]] || fail "${c[9]} ${c[11]} ${c[3]} [${c[30]}] status=${c[27]} check=${c[28]} notes=${c[29]}"
    [[ "${c[16]}" =~ ^[0-9]+$ && "${c[16]}" -gt 0 ]] || fail "${c[9]} ${c[11]} ${c[3]} min_ns=${c[16]} (elided?)"
    [[ "${c[29]}" == *smoke* ]] || fail "${c[9]} ${c[11]} ${c[3]} notes do not name the smoke size"
done <<< "$OUT"

has() {   # has <lang> <lib> <slug> [variant]
    echo "$OUT" | awk -F, -v l="$1" -v lib="$2" -v b="$3" -v v="${4:-}" '$10==l && $12==lib && $4==b && (v=="" || $31==v)' \
        | grep -q . || fail "no $1 $2 wire/$3${4:+ [$4]} row"
}
for lib in protobuf flatbuffers capnproto; do has cpp "$lib" encoder-encode; has cpp "$lib" encoder-decode; done
for lib in flatbuffers capnproto; do has cpp "$lib" struct-view-read; done
for lib in protobuf capnproto; do has cpp "$lib" schemaencoder-encode; has cpp "$lib" schemaencoder-decode; done
has cpp zlib compressstream-roundtrip zlib; has cpp zstd compressstream-roundtrip zstd
has java protobuf-java encoder-encode; has java protobuf-java encoder-decode
has java avro schemaencoder-encode; has java avro schemaencoder-decode
has java java.util.zip compressstream-roundtrip zlib; has java zstd-jni compressstream-roundtrip zstd
has rust bincode encoder-encode; has rust bincode encoder-decode
has rust flate2 compressstream-roundtrip zlib; has rust zstd compressstream-roundtrip zstd
has go compress/flate compressstream-roundtrip zlib; has go klauspost/compress/zstd compressstream-roundtrip zstd

ids="$(grep -ho '^| `wire/[a-z0-9-]*`' "$PROJ"/wire/*.md | sed 's/.*wire\///; s/`//' | sort -u)"
for id in $ids; do
    echo "$OUT" | awk -F, -v b="$id" '$4==b' | grep -q . || fail "page id wire/$id has no competitor row"
done
echo "[wire-comp] $(echo "$OUT" | grep -c .) rows ok across $(echo "$OUT" | awk -F, '{print $10}' | sort -u | tr '\n' ' ')"
echo "WIRE-COMP OK ($(echo "$ids" | wc -w) page ids)"
