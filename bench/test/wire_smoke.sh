#!/usr/bin/env bash
# wire/ smoke (benchmark-reference unit 13, plan 13.1.1): Encoder and SchemaEncoder
# encode/decode of 1e6 100-field records, the zero-copy struct view read, and the
# CompressStream round-trip.
# Every id on the wire pages (bench/wire/**/*.md) emits a row per variant with status
# ok, check_ok true, a positive min_ns, the catalog id wire/<slug> and the smoke tag,
# under CAJETA_BENCH_SMOKE=1 (reduced sizes; each benchmark says which in notes).
#
# Env: CAJETA (compiler, default <repo>/build/src/cajeta), CAJETA_BENCH_OUT
# (directory for the CSV and logs, default /tmp), NO_BUILD=1 (reuse build/bench).
set -euo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
PROJ="$( cd -- "${SCRIPT_DIR}/.." &> /dev/null && pwd )"
REPO_ROOT="$( cd -- "${PROJ}/.." &> /dev/null && pwd )"
CAJETA="${CAJETA:-${REPO_ROOT}/build/src/cajeta}"
export CAJETA
OUTD="${CAJETA_BENCH_OUT:-/tmp}"; mkdir -p "$OUTD"

fail() { echo "WIRE FAIL: $1" >&2; exit 1; }
[[ -x "$CAJETA" ]] || fail "cajeta not found at $CAJETA"

cd "$PROJ"
if [[ "${NO_BUILD:-0}" != 1 ]]; then
    echo "[wire] building (release)..."
    t0=$SECONDS
    "$CAJETA" release >"$OUTD/wire-build.log" 2>&1 || { tail -30 "$OUTD/wire-build.log"; fail "release build failed"; }
    echo "[wire] build $((SECONDS - t0)) s"
fi

OUT="$OUTD/cajeta-bench-wire.csv"
LOG="$OUTD/wire-run.log"
rm -f "$OUT"
t0=$SECONDS
CAJETA_BENCH_SMOKE=1 PROFILE_RUN_ID=wire-run "$PROJ/build/bench" --run --area wire --out "$OUT" >"$LOG" 2>&1 \
    || { tail -30 "$LOG"; fail "--run --area wire exited non-zero"; }
echo "[wire] run $((SECONDS - t0)) s"
grep -hE "^  (ok|skipped|invalid|failed)  " "$LOG" | sed 's/^/  /' || true

rows_for() { awk -F, -v b="$1" -v v="$2" 'NR>1 && $4==b && (v=="" || $31==v)' "$OUT"; }
check() {   # check <slug> [variant]
    local b="$1" v="${2:-}" rows row
    rows="$(rows_for "$b" "$v")"
    [[ -n "$rows" ]] || fail "no row for wire/$b${v:+ [$v]}"
    while IFS= read -r row; do
        IFS=, read -r -a c <<< "$row"
        [[ "${c[4]}/${c[3]}" == "wire/$b" ]] || fail "$b id=${c[4]}/${c[3]} (want wire/$b)"
        [[ "${c[27]}" == "ok" ]]   || fail "wire/$b${v:+ [$v]} status=${c[27]} notes=${c[29]}"
        [[ "${c[28]}" == "true" ]] || fail "wire/$b${v:+ [$v]} check_ok=${c[28]} notes=${c[29]}"
        [[ "${c[16]}" =~ ^[0-9]+$ && "${c[16]}" -gt 0 ]] || fail "wire/$b${v:+ [$v]} min_ns=${c[16]} (elided?)"
        [[ "${c[29]}" == *smoke* ]] || fail "wire/$b row not tagged smoke"
        echo "[wire] wire/$b${v:+ [$v]} ok  min_ns=${c[16]}"
    done <<< "$rows"
}
check_skip() {   # check_skip <slug> <variant>: a skipped row with a reason
    local row
    row="$(rows_for "$1" "$2")"
    [[ -n "$row" ]] || fail "no row for wire/$1 [$2]"
    IFS=, read -r -a c <<< "$row"
    [[ "${c[27]}" == "skipped" && -n "${c[29]}" ]] || fail "wire/$1 [$2] status=${c[27]} (want skipped with a reason)"
    echo "[wire] wire/$1 [$2] skipped: ${c[29]}"
}

check encoder-encode; check encoder-decode; check struct-view-read
check schemaencoder-encode; check schemaencoder-decode
check compressstream-roundtrip zlib
check_skip compressstream-roundtrip zstd   # no zstd in the stdlib or dev.cajeta.codec: a reasoned skip

# Every id on the pages is covered (a page row added later without a benchmark fails here).
ids="$(cat "$PROJ"/wire/*.md $(ls "$PROJ"/wire/*/*.md 2>/dev/null) | grep -o '^| `wire/[a-z0-9-]*`' | sed 's/.*wire\///; s/`//' | sort -u)"
for id in $ids; do
    awk -F, -v b="$id" 'NR>1 && $4==b' "$OUT" | grep -q . || fail "page id wire/$id has no row"
done
others="$(awk -F, 'NR>1 && $5 != "wire"' "$OUT" | wc -l)"
[[ "$others" == "0" ]] || fail "--area wire emitted $others non-wire rows"

echo "WIRE OK ($(echo "$ids" | wc -w) page ids)"
