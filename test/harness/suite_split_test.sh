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

echo
echo "PASS=$pass FAIL=$fail"
[ "$fail" -eq 0 ]
