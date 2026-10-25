#!/usr/bin/env bash
# aot/ competitor smoke (benchmark-reference unit 13, plan 13.1.2): run.sh aot
# under CAJETA_BENCH_SMOKE=1 emits, for aot/di-graph-100-startup (the page's one
# id), a schema-v2 row per framework the page names (Spring, Dagger, Guice: java;
# google/wire: go; shaku: rust), each status ok (shaku: skipped, because the
# graph does not compile on shaku in practical time; the driver says why), check_ok true (the program
# printed components=100 value=<V> of competitors/cpp/aot/expect.txt) and a
# positive min_ns. It also checks the generated sources are current: re-running
# scripts/gen_di_graph.py into a scratch tree reproduces every committed file.
set -uo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
PROJ="$( cd -- "${SCRIPT_DIR}/.." &> /dev/null && pwd )"
[[ -d "$HOME/.cargo/bin" ]] && export PATH="$HOME/.cargo/bin:$PATH"
OUTD="${CAJETA_BENCH_OUT:-/tmp}"; mkdir -p "$OUTD"
fail() { echo "AOT-COMP FAIL: $1" >&2; exit 1; }

# (0) the committed graph sources are the generator's output
GEN="$(mktemp -d "$OUTD/aot-gen.XXXXXX")" || fail "mktemp"
python3 "$PROJ/scripts/gen_di_graph.py" "$GEN" >/dev/null || fail "gen_di_graph.py failed"
while IFS= read -r f; do
    rel="${f#$GEN/}"
    cmp -s "$f" "$PROJ/$rel" || fail "$rel differs from gen_di_graph.py's output (re-run scripts/gen_di_graph.py)"
done < <(find "$GEN" -type f)
echo "[aot-comp] generated sources current ($(find "$GEN" -type f | wc -l) files)"

t0=$SECONDS
OUT="$(cd "$PROJ/competitors" && CAJETA_BENCH_SMOKE=1 PROFILE_RUN_ID=aot-comp PROFILE_RUN_TS=0 PROFILE_TRIALS=3 PROFILE_WARMUP=1 \
       bash run.sh aot 2>"$OUTD/aot-comp.err")" || fail "run.sh aot exited non-zero (see $OUTD/aot-comp.err)"
echo "[aot-comp] run $((SECONDS - t0)) s"
[[ -n "$OUT" ]] || fail "run.sh aot produced no rows"
while IFS= read -r row; do
    [[ -z "$row" ]] && continue
    IFS=, read -r -a c <<< "$row"
    [[ "${#c[@]}" -ge 36 && "${c[0]}" == "2" && "${c[4]}" == "aot" ]] || fail "row not v2 aot: $row"
    [[ "${c[3]}" == "di-graph-100-startup" ]] || fail "unexpected id aot/${c[3]}"
    if [[ "${c[9]} ${c[11]} ${c[27]}" == "rust shaku skipped" && "${c[29]}" == *"does not compile"* ]]; then
        echo "[aot-comp] rust shaku skipped (justified: ${c[29]%%;*})"; continue
    fi
    [[ "${c[27]}" == "ok" && "${c[28]}" == "true" ]] || fail "${c[9]} ${c[11]} status=${c[27]} check=${c[28]} notes=${c[29]}"
    [[ "${c[16]}" =~ ^[0-9]+$ && "${c[16]}" -gt 0 ]] || fail "${c[9]} ${c[11]} min_ns=${c[16]}"
    echo "[aot-comp] ${c[9]} ${c[11]} ${c[12]} ok  min_ns=${c[16]}"
done <<< "$OUT"
for want in "java spring" "java dagger" "java guice" "go wire" "rust shaku"; do
    set -- $want
    echo "$OUT" | awk -F, -v l="$1" -v b="$2" '$10==l && $12==b && $4=="di-graph-100-startup"' | grep -q . || fail "no $1 $2 row"
done
ids="$(grep -ho '^| `aot/[a-z0-9-]*`' "$PROJ"/aot/*.md | sed 's/.*aot\///; s/`//' | sort -u)"
for id in $ids; do
    echo "$OUT" | awk -F, -v b="$id" '$4==b' | grep -q . || fail "page id aot/$id has no competitor row"
done
echo "AOT-COMP OK ($(echo "$OUT" | grep -c .) rows)"
