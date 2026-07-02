#!/bin/bash
# Tests for cajeta_tests.sh's chunked (split-oversized-suite) scheduler.
#
# These exercise the SCHEDULING math only — they never build or run the real
# gtest binary. Two seams in cajeta_tests.sh make that possible:
#   CAJETA_TESTS_FILE=<f>  inject the discovered test list (one Suite.test/line),
#                          bypassing --gtest_list_tests and the build guards.
#   PLAN_ONLY=1            after bin-packing, print the chunk->shard plan and
#                          exit 0 before spawning any worker.
#
# PLAN_ONLY output contract (stdout), one PLAN header then one line per unit:
#   PLAN shards=<S> batch=<0|1> batch_max=<N> chunks=<C> tests=<T>
#   CHUNK <idx> shard=<s> count=<k> name=<display> tests=<t1,t2,...>
# (In BATCH=0 each unit is a single test: count=1, name=<Suite.test>.)
#
# Unit 1.1 of agents/test-harness-suite-split-plan.md. bash-3.2 compatible.

set -u
HARNESS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HARNESS_DIR/../.." && pwd)"
CT="$ROOT/cajeta_tests.sh"
TMP="$(mktemp -d -t suite_split_test.XXXXXX)"
trap 'rm -rf "$TMP"' EXIT

pass=0; fail=0
ok()   { pass=$((pass+1)); printf '  ok   %s\n' "$1"; }
bad()  { fail=$((fail+1)); printf '  FAIL %s\n' "$1"; [ -n "${2:-}" ] && printf '        %s\n' "$2"; }
check(){ if [ "$2" = "$3" ]; then ok "$1"; else bad "$1" "expected [$3] got [$2]"; fi; }

# Write a canned test list: gen_list <file> <Suite> <count> [<Suite2> <count2> ...]
gen_list() {
    local f="$1"; shift
    : > "$f"
    while [ "$#" -ge 2 ]; do
        local s="$1" n="$2"; shift 2
        local i=1
        while [ "$i" -le "$n" ]; do
            printf '%s.t%02d\n' "$s" "$i" >> "$f"
            i=$((i+1))
        done
    done
}

# Run PLAN_ONLY and echo stdout. run_plan <listfile> <shards> [env assignments...]
run_plan() {
    local lf="$1" sh="$2"; shift 2
    env "$@" CAJETA_TESTS_FILE="$lf" PLAN_ONLY=1 "$CT" "shard=$sh" 2>/dev/null
}

# --- 1.1.2 split math: 40 tests, BATCH_MAX=16 -> 3 chunks (16/16/8) ---------
LF="$TMP/big.txt"; gen_list "$LF" BigSuite 40
PLAN="$(run_plan "$LF" 4 BATCH_MAX=16)"
chunks=$(printf '%s\n' "$PLAN" | grep -c '^CHUNK ')
check "1.1.2 chunk count = ceil(40/16)=3" "$chunks" "3"
counts=$(printf '%s\n' "$PLAN" | sed -nE 's/^CHUNK .* count=([0-9]+) .*/\1/p' | sort -rn | paste -sd, -)
check "1.1.2 chunk sizes 16,16,8" "$counts" "16,16,8"
# Contiguity + union: concat every chunk's tests in chunk-index order == 1..40.
union=$(printf '%s\n' "$PLAN" | grep '^CHUNK ' | sort -n -k2.7 \
        | sed -nE 's/^CHUNK.* tests=(.*)$/\1/p' | tr ',' '\n' | grep -c .)
check "1.1.2 union covers all 40 tests" "$union" "40"
dupes=$(printf '%s\n' "$PLAN" | sed -nE 's/^CHUNK.* tests=(.*)$/\1/p' | tr ',' '\n' \
        | sort | uniq -d | grep -c .)
check "1.1.2 no test appears in two chunks" "$dupes" "0"

# --- 1.1.3 parity: every suite <= BATCH_MAX -> one chunk per suite ----------
LF="$TMP/small.txt"; gen_list "$LF" SmallA 3 SmallB 5 SmallC 10
PLAN="$(run_plan "$LF" 4 BATCH_MAX=16)"
chunks=$(printf '%s\n' "$PLAN" | grep -c '^CHUNK ')
check "1.1.3 one chunk per suite (3 suites)" "$chunks" "3"
names=$(printf '%s\n' "$PLAN" | sed -nE 's/^CHUNK .* name=([^ ]+).*/\1/p' | sort | paste -sd, -)
check "1.1.3 chunk names are the bare suites" "$names" "SmallA,SmallB,SmallC"

# --- 1.1.4 distribution: split suite lands on >=2 distinct shards -----------
LF="$TMP/big.txt"; gen_list "$LF" BigSuite 40
PLAN="$(run_plan "$LF" 4 BATCH_MAX=16)"
nshards=$(printf '%s\n' "$PLAN" | sed -nE 's/^CHUNK .* shard=([0-9]+) .*/\1/p' | sort -u | grep -c .)
if [ "$nshards" -ge 2 ]; then ok "1.1.4 chunks span >=2 shards ($nshards)"; else bad "1.1.4 chunks span >=2 shards" "got $nshards"; fi

# --- 1.1.5 BATCH_MAX override changes chunk count ---------------------------
LF="$TMP/big.txt"; gen_list "$LF" BigSuite 40
c8=$(run_plan "$LF" 8 BATCH_MAX=8 | grep -c '^CHUNK ')
check "1.1.5 BATCH_MAX=8 on 40 -> 5 chunks" "$c8" "5"
cdef=$(run_plan "$LF" 8 | grep -c '^CHUNK ')   # unset -> default 16 -> 3
check "1.1.5 default BATCH_MAX=16 -> 3 chunks" "$cdef" "3"

# --- 1.1.6 BATCH=0 ignores chunking (one unit per test) ---------------------
LF="$TMP/mix.txt"; gen_list "$LF" A 5 B 7   # 12 tests
PLAN="$(run_plan "$LF" 4 BATCH=0)"
units=$(printf '%s\n' "$PLAN" | grep -c '^CHUNK ')
check "1.1.6 BATCH=0 -> one unit per test (12)" "$units" "12"
allone=$(printf '%s\n' "$PLAN" | sed -nE 's/^CHUNK .* count=([0-9]+) .*/\1/p' | sort -u | paste -sd, -)
check "1.1.6 BATCH=0 units all count=1" "$allone" "1"

# ===========================================================================
# Unit 2 — executor: chunks actually run; parity + crash attribution.
# Uses a STUB binary (fast, deterministic — no JIT/stdlib prime, no LLVM DLL)
# so the real executor path (run_suite_batch, fallback, aggregation) is exercised
# without building or running the real 1.2GB cajeta_test. Test-name conventions
# the stub honors: *CRASHME* -> segfault (exit 139, no completion line);
# *FAILME* -> a gtest [ FAILED ] line; everything else passes.
STUB="$TMP/cajeta_test_stub"
cat > "$STUB" <<'STUBEOF'
#!/bin/bash
filter=""
for a in "$@"; do case "$a" in --gtest_filter=*) filter="${a#--gtest_filter=}";; esac; done
IFS=':' read -ra tlist <<< "$filter"
pass=0; fails=()
for t in "${tlist[@]}"; do
    [ -z "$t" ] && continue
    case "$t" in
        *CRASHME*) echo "[ RUN      ] $t"; exit 139 ;;   # no "ran." line -> batch fallback
        *FAILME*)  fails+=("$t") ;;
        *)         pass=$((pass+1)) ;;
    esac
done
for f in "${fails[@]}"; do echo "[  FAILED  ] $f (1 ms)"; done
echo "[==========] ${#tlist[@]} tests from 1 test suite ran. (5 ms total)"
echo "[  PASSED  ] $pass tests."
[ ${#fails[@]} -gt 0 ] && exit 1
exit 0
STUBEOF
chmod +x "$STUB"

# run_exec <listfile> <shards> [env...] -> full stdout of a real (non-PLAN) run
run_exec() {
    local lf="$1" sh="$2"; shift 2
    env "$@" TUI=0 CAJETA_TESTS_FILE="$lf" TEST_BIN="$STUB" "$CT" "shard=$sh" 2>&1
}
sumline() { printf '%s\n' "$1" | grep -E '^Passed: '; }

# --- 2.1.1 parity: split vs unsplit report identical tallies ----------------
LF="$TMP/par.txt"; gen_list "$LF" BigSuite 20
SPLIT="$(run_exec "$LF" 4 BATCH_MAX=8   || true)"    # 20 -> chunks 8,8,4
WHOLE="$(run_exec "$LF" 4 BATCH_MAX=100 || true)"    # 20 -> 1 chunk
s_sum="$(sumline "$SPLIT")"; w_sum="$(sumline "$WHOLE")"
check "2.1.1 split summary = unsplit summary" "$s_sum" "$w_sum"
check "2.1.1 all 20 passed (split)" "$s_sum" "Passed: 20   Failed: 0   Timed out: 0   Crashed: 0   Skipped/disabled: 0"

# --- 2.1.2 crash attribution + fallback bounded to the crash's chunk --------
LF="$TMP/crash.txt"
{ for i in 01 02; do echo "MixSuite.t$i"; done; echo "MixSuite.CRASHME"; \
  for i in 04 05 06 07 08 09 10; do echo "MixSuite.t$i"; done; } > "$LF"   # 10 tests
OUT="$(run_exec "$LF" 2 BATCH_MAX=4 VERBOSE=1 || true)"                    # chunks: 4,4,2
csum="$(sumline "$OUT")"
check "2.1.2 9 pass / 1 crash" "$csum" "Passed: 9   Failed: 0   Timed out: 0   Crashed: 1   Skipped/disabled: 0"
attr=$(printf '%s\n' "$OUT" | grep -c '^>>> CRASH MixSuite\.CRASHME ')
check "2.1.2 crash attributed to MixSuite.CRASHME by name" "$attr" "1"
nfb=$(printf '%s\n' "$OUT" | grep -c '^>>> BATCH-FALLBACK ')
check "2.1.2 exactly one chunk fell back (the crash chunk)" "$nfb" "1"
fbname=$(printf '%s\n' "$OUT" | grep '^>>> BATCH-FALLBACK ' | grep -c 'MixSuite\[1/3\]')
check "2.1.2 the fallback chunk is MixSuite[1/3]" "$fbname" "1"

# --- 2.1.2b a FAILME assertion is counted + attributed (not a crash) --------
LF="$TMP/fail.txt"
{ echo "FSuite.ok1"; echo "FSuite.FAILME"; echo "FSuite.ok2"; } > "$LF"
OUT="$(run_exec "$LF" 2 BATCH_MAX=8 || true)"
fsum="$(sumline "$OUT")"
check "2.1.2b 2 pass / 1 fail / 0 crash" "$fsum" "Passed: 2   Failed: 1   Timed out: 0   Crashed: 0   Skipped/disabled: 0"

# --- 2.3.3 BATCH=0 regression: one process per test; run_one_test still gets
# the real test id (not a chunk index) after the chunk-dispatch rewrite. --------
LF="$TMP/b0.txt"; gen_list "$LF" ZSuite 6
OUT="$(run_exec "$LF" 3 BATCH=0 || true)"
check "2.3.3 BATCH=0 runs all 6, all pass" "$(sumline "$OUT")" \
      "Passed: 6   Failed: 0   Timed out: 0   Crashed: 0   Skipped/disabled: 0"
OUT="$(run_exec "$LF" 3 BATCH=0 VERBOSE=1 || true)"
bc=$(printf '%s\n' "$OUT" | grep -cE '^>> ZSuite\.t[0-9]+ \.\.\. PASS$')
check "2.3.3 BATCH=0 emits a per-test breadcrumb for each real test id (6)" "$bc" "6"

echo
echo "PASS=$pass FAIL=$fail"
[ "$fail" -eq 0 ]
