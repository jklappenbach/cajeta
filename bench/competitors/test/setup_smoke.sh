#!/usr/bin/env bash
# Smoke test for competitors/setup.sh (benchmark-reference plan 20.1.1).
#
#  1. After setup.sh, every pinned item it knows (`setup.sh --list-items`) has a
#     row in toolchains-<machine>.tsv, and no row is plain `missing`. Rows marked
#     `missing (needs sudo)` are reported; SMOKE_STRICT=1 makes them fail too.
#  2. A deliberately missing pin, injected with CAJETA_SETUP_INJECT_MISSING,
#     makes `setup.sh --verify` exit non-zero and name the item, for one
#     toolchain (wrk) and one pinned library (simdjson).
#
# SMOKE_NO_INSTALL=1 runs step 1 as `setup.sh --verify` (no installs), for a box
# already provisioned. Exit 0 = pass.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SETUP="$HERE/../setup.sh"
MACHINE="${CAJETA_SETUP_MACHINE:-$(hostname | tr '[:upper:]' '[:lower:]')}"
TSV="$HERE/../toolchains-${MACHINE}.tsv"
SCRATCH="$(mktemp -d "${TMPDIR:-/tmp}/setup_smoke.XXXXXX")"
trap 'rm -rf "$SCRATCH"' EXIT

fails=0
fail() { echo "FAIL: $*"; fails=$((fails + 1)); }
pass() { echo "ok:   $*"; }

[[ -x "$SETUP" ]] || { echo "FAIL: setup.sh not found or not executable: $SETUP"; exit 1; }

# ---- 1. provisioned: every item found and recorded ---------------------------
mode=()
[[ "${SMOKE_NO_INSTALL:-0}" == 1 ]] && mode=(--verify)
if "$SETUP" "${mode[@]}" >"$SCRATCH/setup.out" 2>&1; then
    pass "setup.sh ${mode[*]} exited 0"
else
    fail "setup.sh ${mode[*]} exited non-zero; tail of output:"; tail -20 "$SCRATCH/setup.out"
fi

[[ -f "$TSV" ]] || { fail "no TSV written at $TSV"; echo "$fails failure(s)"; exit 1; }
head -1 "$TSV" | grep -q $'^item\tkind\tversion\tpath\tverified$' \
    && pass "TSV header" || fail "TSV header is not item/kind/version/path/verified: $(head -1 "$TSV")"

mapfile -t items < <("$SETUP" --list-items)
(( ${#items[@]} > 20 )) && pass "${#items[@]} pinned items listed" \
    || fail "setup.sh --list-items listed only ${#items[@]} items"
for it in "${items[@]}"; do
    row="$(awk -F'\t' -v n="$it" '$1==n' "$TSV")"
    if [[ -z "$row" ]]; then fail "item not recorded in TSV: $it"; continue; fi
    ver="$(cut -f3 <<<"$row")"
    case "$ver" in
        "missing (needs sudo)")
            if [[ "${SMOKE_STRICT:-0}" == 1 ]]; then fail "needs sudo: $it"; else echo "warn: needs sudo: $it"; fi ;;
        missing*|"") fail "pinned item missing: $it ($ver)" ;;
    esac
done
for k in toolchain library checkout; do
    awk -F'\t' -v k="$k" 'NR>1 && $2==k {f=1} END {exit !f}' "$TSV" \
        && pass "TSV has $k rows" || fail "TSV has no $k rows"
done

# ---- 2. an injected missing pin fails, naming the item ----------------------
for victim in wrk simdjson; do
    out="$SCRATCH/inject-$victim.out"
    if CAJETA_SETUP_INJECT_MISSING="$victim" CAJETA_SETUP_TSV="$SCRATCH/inject-$victim.tsv" \
            "$SETUP" --verify >"$out" 2>&1; then
        fail "setup.sh --verify passed with $victim injected missing"
    else
        grep -q "missing pin: $victim" "$out" && pass "injected $victim: failed naming it" \
            || { fail "injected $victim: failed but did not name it"; tail -5 "$out"; }
    fi
    awk -F'\t' -v n="$victim" '$1==n && $3 ~ /^missing/ {f=1} END {exit !f}' "$SCRATCH/inject-$victim.tsv" 2>/dev/null \
        && pass "injected $victim: TSV row marked missing" || fail "injected $victim: TSV row not marked missing"
done

echo
if (( fails == 0 )); then echo "setup_smoke: PASS"; exit 0; fi
echo "setup_smoke: $fails failure(s)"; exit 1
