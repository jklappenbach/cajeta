#!/usr/bin/env bash
# hash/ competitor smoke (benchmark-reference unit 8, spec §6.5). Runs every
# competitors/drivers/hash/*.sh program (`hash`: xxhash3 x4 and siphash;
# `hasher`: defaulthasher-string, defaulthasher-int, hash-struct) and asserts:
#   (1) every row is schema v2 with the catalog id, machine, os and platform;
#   (2) each id on bench/hash/*.md has an ok, check_ok=true row from every
#       competitor library its page names (table below) — for xxhash3 and
#       siphash that is the reference digest every language checks;
#   (3) anti-elision: an ok row of N hashes took at least N/10 ns (the 256k
#       rows count bytes/16, a 160 GB/s ceiling);
#   (4) any skipped row carries a reason.
# Env: PROFILE_TRIALS (3 here), PROFILE_WARMUP (1 here), CAJETA_BENCH_OUT
# (where the rows and stderr land; default /tmp).
set -uo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
source "$SCRIPT_DIR/csv_lib.sh"   # cawk / csv_fields: CSV-aware column checks
PROJ="$( cd -- "${SCRIPT_DIR}/.." &> /dev/null && pwd )"
RUNNER="${PROJ}/competitors/hash.sh"
NCOLS="$(cawk '{print NF; exit}' "${PROJ}/competitors/columns.txt")"
OUTDIR="${CAJETA_BENCH_OUT:-/tmp}"; mkdir -p "$OUTDIR"

fail() { echo "HASH-COMP FAIL: $1" >&2; exit 1; }

echo "[hash-comp] building + running hash competitors (cargo, g++, go, javac on first run)..."
t0=$SECONDS
OUT="$(PROFILE_RUN_ID=hash-comp PROFILE_RUN_TS=0 PROFILE_TRIALS="${PROFILE_TRIALS:-3}" PROFILE_WARMUP="${PROFILE_WARMUP:-1}" \
        bash "$RUNNER" 2>"$OUTDIR/hash-comp.err")" || fail "hash.sh exited non-zero (see $OUTDIR/hash-comp.err)"
printf '%s\n' "$OUT" > "$OUTDIR/hash-comp.csv"
echo "[hash-comp] ran in $((SECONDS - t0))s"
[[ -n "$OUT" ]] || fail "hash.sh produced no rows"

REQUIRED='
xxhash3 10000000 cpp/xxHash rust/xxhash-rust go/zeebo/xxh3 java/zero-allocation-hashing python/xxhash
xxhash3-256k 163840000 cpp/xxHash rust/xxhash-rust go/zeebo/xxh3 java/zero-allocation-hashing python/xxhash
xxhash3_128 10000000 cpp/xxHash rust/xxhash-rust go/zeebo/xxh3 java/zero-allocation-hashing python/xxhash
xxhash3_128-256k 163840000 cpp/xxHash rust/xxhash-rust go/zeebo/xxh3 java/zero-allocation-hashing python/xxhash
siphash 10000000 cpp/siphash-reference rust/siphasher go/dchest/siphash java/guava python/siphash24
defaulthasher-string 10000000 rust/std go/maphash java/java.base python/builtins
defaulthasher-int 100000000 rust/std go/maphash java/java.base python/builtins
hash-struct 10000000 cpp/boost::hash_combine rust/std go/maphash java/java.util.Objects
'
IDS="$(awk 'NF{print $1}' <<<"$REQUIRED" | tr '\n' ' ')"

while IFS= read -r row; do
    [[ -z "$row" ]] && continue
    N="$(cawk '{print NF}' <<<"$row")"
    [[ "$N" == "$NCOLS" ]] || fail "row has $N cols, expected $NCOLS: $row"
    cawk '$1!="2" || $5!="hash" || $33=="" || $34=="" || $37=="" || $37!=$10 {exit 1}' <<<"$row" \
        || fail "row is not v2 hash/ with machine/os/platform: $row"
    b="$(cawk '{print $4}' <<<"$row")"
    case " $IDS " in *" $b "*) ;; *) fail "unexpected benchmark: $b" ;; esac
    st="$(cawk '{print $28}' <<<"$row")"
    if [[ "$st" == "skipped" ]]; then
        [[ -n "$(cawk '{print $30}' <<<"$row")" ]] || fail "skip row missing reason: $row"
    elif [[ "$st" != "ok" ]]; then
        fail "status '$st': $row"
    fi
done < <(csv_fields <<<"$OUT")

while read -r b ops libs; do
    [[ -z "$b" ]] && continue
    for want in $libs; do
        lang="${want%%/*}"; lib="${want#*/}"
        r="$(cawk -v b="$b" -v l="$lang" -v lib="$lib" '$4==b && $10==l && $12==lib' <<<"$OUT" | head -1)"
        [[ -n "$r" ]] || fail "no $lang $lib row for hash/$b"
        [[ "$(cawk '{print $28 "/" $29}' <<<"$r")" == "ok/true" ]] \
            || fail "$lang $lib hash/$b not ok/check_ok: $(cawk '{print $28, $29, $30}' <<<"$r")"
        min="$(cawk '{print $17}' <<<"$r")"
        (( min >= ops / 10 )) || fail "$lang $lib hash/$b min_ns=$min for $ops hashes looks elided"
    done
    echo "[hash-comp] $b: $(cawk -v b="$b" '$4==b && $28=="ok" {printf "%s/%s ", $10, $12}' <<<"$OUT")"
done <<< "$REQUIRED"

echo "HASH-COMP OK"
