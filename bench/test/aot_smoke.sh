#!/usr/bin/env bash
# aot/ smoke (benchmark-reference unit 13, plan 13.1.1): start-up of a process with a
# 100-component DI graph, 10 deep, to its first injected call.
# Every id on the aot pages (bench/aot/**/*.md) emits a row per variant with status
# ok, check_ok true, a positive min_ns, the catalog id aot/<slug> and the smoke tag,
# under CAJETA_BENCH_SMOKE=1 (reduced sizes; each benchmark says which in notes).
#
# Env: CAJETA (compiler, default <repo>/build/src/cajeta), CAJETA_BENCH_OUT
# (directory for the CSV and logs, default /tmp), NO_BUILD=1 (reuse build/bench).
set -euo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
source "$SCRIPT_DIR/csv_lib.sh"   # cawk / csv_fields: CSV-aware column checks
PROJ="$( cd -- "${SCRIPT_DIR}/.." &> /dev/null && pwd )"
REPO_ROOT="$( cd -- "${PROJ}/.." &> /dev/null && pwd )"
CAJETA="${CAJETA:-${REPO_ROOT}/build/src/cajeta}"
export CAJETA
OUTD="${CAJETA_BENCH_OUT:-/tmp}"; mkdir -p "$OUTD"

fail() { echo "AOT FAIL: $1" >&2; exit 1; }
[[ -x "$CAJETA" ]] || fail "cajeta not found at $CAJETA"

cd "$PROJ"
if [[ "${NO_BUILD:-0}" != 1 ]]; then
    echo "[aot] building (release)..."
    t0=$SECONDS
    "$CAJETA" release >"$OUTD/aot-build.log" 2>&1 || { tail -30 "$OUTD/aot-build.log"; fail "release build failed"; }
    echo "[aot] build $((SECONDS - t0)) s"
fi

OUT="$OUTD/cajeta-bench-aot.csv"
LOG="$OUTD/aot-run.log"
rm -f "$OUT"
t0=$SECONDS
CAJETA_BENCH_SMOKE=1 PROFILE_RUN_ID=aot-run "$PROJ/build/bench" --run --area aot --out "$OUT" >"$LOG" 2>&1 \
    || { tail -30 "$LOG"; fail "--run --area aot exited non-zero"; }
echo "[aot] run $((SECONDS - t0)) s"
grep -hE "^  (ok|skipped|invalid|failed)  " "$LOG" | sed 's/^/  /' || true

rows_for() { cawk -v b="$1" -v v="$2" 'NR>1 && $4==b && (v=="" || $31==v)' "$OUT"; }
check() {   # check <slug> [variant]
    local b="$1" v="${2:-}" rows row
    rows="$(rows_for "$b" "$v")"
    [[ -n "$rows" ]] || fail "no row for aot/$b${v:+ [$v]}"
    while IFS= read -r row; do
        IFS="$CSV_FS" read -r -a c <<< "$row"
        [[ "${c[4]}/${c[3]}" == "aot/$b" ]] || fail "$b id=${c[4]}/${c[3]} (want aot/$b)"
        [[ "${c[27]}" == "ok" ]]   || fail "aot/$b${v:+ [$v]} status=${c[27]} notes=${c[29]}"
        [[ "${c[28]}" == "true" ]] || fail "aot/$b${v:+ [$v]} check_ok=${c[28]} notes=${c[29]}"
        [[ "${c[16]}" =~ ^[0-9]+$ && "${c[16]}" -gt 0 ]] || fail "aot/$b${v:+ [$v]} min_ns=${c[16]} (elided?)"
        [[ "${c[29]}" == *smoke* ]] || fail "aot/$b row not tagged smoke"
        echo "[aot] aot/$b${v:+ [$v]} ok  min_ns=${c[16]}"
    done <<< "$rows"
}
check_skip() {   # check_skip <slug> <variant>: a skipped row with a reason
    local row
    row="$(rows_for "$1" "$2")"
    [[ -n "$row" ]] || fail "no row for aot/$1 [$2]"
    IFS="$CSV_FS" read -r -a c <<< "$row"
    [[ "${c[27]}" == "skipped" && -n "${c[29]}" ]] || fail "aot/$1 [$2] status=${c[27]} (want skipped with a reason)"
    echo "[aot] aot/$1 [$2] skipped: ${c[29]}"
}

check di-graph-100-startup

# Every id on the pages is covered (a page row added later without a benchmark fails here).
ids="$(cat "$PROJ"/aot/*.md $(ls "$PROJ"/aot/*/*.md 2>/dev/null) | grep -o '^| `aot/[a-z0-9-]*`' | sed 's/.*aot\///; s/`//' | sort -u)"
for id in $ids; do
    cawk -v b="$id" 'NR>1 && $4==b' "$OUT" | grep -q . || fail "page id aot/$id has no row"
done
others="$(cawk 'NR>1 && $5 != "aot"' "$OUT" | wc -l)"
[[ "$others" == "0" ]] || fail "--area aot emitted $others non-aot rows"

echo "AOT OK ($(echo "$ids" | wc -w) page ids)"
