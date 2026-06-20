#!/usr/bin/env bash
# Phase B concurrency smoke test — cross-language atomic-fetchadd (1M) +
# task-spawn-await (20K) at the same sizes as the Cajeta side. Asserts
# competitors/concurrent.sh emits schema-conformant rows for the present
# languages (rust/cpp std::thread, go goroutine, python asyncio), that every
# measured row passes its exact closed-form cross-check, that python's atomic-
# fetchadd is a `skipped` row with a reason (no lock-free atomic in CPython), and
# that the goroutine task-spawn competitor is present. No dataset needed.
set -uo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
PROJ="$( cd -- "${SCRIPT_DIR}/.." &> /dev/null && pwd )"
RUNNER="${PROJ}/competitors/concurrent.sh"
NCOLS="$(awk -F, '{print NF; exit}' "${PROJ}/competitors/columns.txt")"

fail() { echo "CONC-COMP FAIL: $1" >&2; exit 1; }

echo "[conc-comp] building + running concurrency competitors (compiles cargo/g++/go on first run)…"
OUT="$(PROFILE_RUN_ID=conc-comp PROFILE_RUN_TS=0 PROFILE_TRIALS=3 bash "$RUNNER" 2>/tmp/conc-comp.err)" \
    || fail "concurrent.sh exited non-zero"
[[ -n "$OUT" ]] || fail "concurrent.sh produced no rows"

# (1) every row schema-conformant; benchmark in the expected set; area=concurrent.
while IFS= read -r row; do
    [[ -z "$row" ]] && continue
    N="$(printf '%s' "$row" | awk -F, '{print NF}')"
    [[ "$N" == "$NCOLS" ]] || fail "row has $N cols, expected $NCOLS: $row"
    bench="$(printf '%s' "$row" | awk -F, '{print $4}')"; area="$(printf '%s' "$row" | awk -F, '{print $5}')"
    case "$bench" in atomic-fetchadd|task-spawn-await) ;; *) fail "unexpected benchmark: $bench" ;; esac
    [[ "$area" == "concurrent" ]] || fail "unexpected area: $area"
    status="$(printf '%s' "$row" | awk -F, '{print $28}')"
    if [[ "$status" == "ok" ]]; then
        min="$(printf '%s' "$row" | awk -F, '{print $17}')"
        chk="$(printf '%s' "$row" | awk -F, '{print $29}')"
        [[ "$min" =~ ^[0-9]+$ && "$min" -gt 0 ]] || fail "ok row min_ns not positive: $row"
        [[ "$chk" == "true" ]]                   || fail "ok row failed closed-form cross-check: $row"
    elif [[ "$status" == "skipped" ]]; then
        note="$(printf '%s' "$row" | awk -F, '{print $30}')"
        [[ -n "$note" ]] || fail "skip row missing reason: $row"
    else
        fail "unexpected status '$status': $row"
    fi
done <<< "$OUT"

# (2) rust + go produced ok rows for both benchmarks (always present).
for lang in rust go; do
    for b in atomic-fetchadd task-spawn-await; do
        echo "$OUT" | awk -F, -v l="$lang" -v b="$b" '$10==l && $4==b && $28=="ok" && $29=="true"' | grep -q . \
            || fail "no ok $lang $b row"
    done
done

# (3) python's atomic-fetchadd is a skip with a reason; its asyncio task-spawn is ok.
echo "$OUT" | awk -F, '$10=="python" && $4=="atomic-fetchadd" && $28=="skipped" && $30!=""' | grep -q . \
    || fail "python atomic-fetchadd should be skipped with a reason"
echo "$OUT" | awk -F, '$10=="python" && $12=="asyncio" && $4=="task-spawn-await" && $28=="ok"' | grep -q . \
    || fail "python asyncio task-spawn competitor missing"

# (4) the goroutine task-spawn competitor is present and ok.
echo "$OUT" | awk -F, '$10=="go" && $12=="goroutine" && $4=="task-spawn-await" && $28=="ok"' | grep -q . \
    || fail "goroutine task-spawn competitor missing"

# (5) report.py ingests the concurrency competitor rows.
TMP="$(mktemp -d)"
{ cat "${PROJ}/competitors/columns.txt"; echo "$OUT"; } > "$TMP/results.csv"
echo "schema_version,key,value" > "$TMP/env.csv"
python3 "${PROJ}/report/report.py" "$TMP" >/dev/null 2>&1 || fail "report.py failed on concurrent rows"
grep -q "task-spawn-await" "$TMP/site/index.html" || fail "report site missing concurrent rows"
rm -rf "$TMP"

NLANGS="$(echo "$OUT" | awk -F, '$28=="ok"{print $10}' | sort -u | tr '\n' ' ')"
echo "[conc-comp] measured langs: [$NLANGS]; goroutine vs OS-thread vs asyncio spawn captured"
echo "CONC-COMP OK"
