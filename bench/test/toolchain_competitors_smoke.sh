#!/usr/bin/env bash
# toolchain/ competitor smoke (benchmark-reference unit 15, spec §6.14). Runs
# every competitors/drivers/toolchain/*.sh program (compile, startup,
# binarysize, buildtool, kernel, debugger) and asserts:
#   (1) every row is schema v2, area toolchain, with machine, os and platform;
#   (2) each id on bench/toolchain/*.md has an ok, check_ok=true row from every
#       competitor its page names (table below), for every variant listed;
#   (3) anti-elision: an ok row's min_ns reaches the id's floor, and a
#       binary-size row's memory column (size on disk, KiB) is positive and
#       says so in its notes;
#   (4) any skipped row carries a reason.
# Expected wall time on Phoenix: about 6 minutes (3 trials each; the evcxr
# kernel is about 20 s a launch).
# Env: PROFILE_TRIALS (3 here), PROFILE_WARMUP (1 here), CAJETA_BENCH_OUT
# (where the rows and stderr land; default /tmp).
set -uo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
source "$SCRIPT_DIR/csv_lib.sh"   # cawk / csv_fields: CSV-aware column checks
PROJ="$( cd -- "${SCRIPT_DIR}/.." &> /dev/null && pwd )"
RUNNER="${PROJ}/competitors/toolchain.sh"
NCOLS="$(cawk '{print NF; exit}' "${PROJ}/competitors/columns.txt")"
OUTDIR="${CAJETA_BENCH_OUT:-/tmp}"; mkdir -p "$OUTDIR"

fail() { echo "TOOLCHAIN-COMP FAIL: $1" >&2; exit 1; }

echo "[toolchain-comp] building + running toolchain competitors from $(date +%H:%M)..."
t0=$SECONDS
OUT="$(PROFILE_RUN_ID=toolchain-comp PROFILE_RUN_TS=0 PROFILE_TRIALS="${PROFILE_TRIALS:-3}" PROFILE_WARMUP="${PROFILE_WARMUP:-1}" \
        bash "$RUNNER" 2>"$OUTDIR/toolchain-comp.err")" || fail "toolchain.sh exited non-zero (see $OUTDIR/toolchain-comp.err)"
printf '%s\n' "$OUT" > "$OUTDIR/toolchain-comp.csv"
echo "[toolchain-comp] ran in $((SECONDS - t0))s"
[[ -n "$OUT" ]] || fail "toolchain.sh produced no rows"

# id variant floor_ns lang/library ...   (variant "-" = none)
REQUIRED='
compile-tour-cold - 100000000 java/javac rust/cargo go/go cpp/g++
compile-tour-warm - 1000000 java/javac rust/cargo go/go cpp/g++
compile-10k - 100000000 java/javac rust/cargo go/go cpp/g++
compile-lto off 100000000 rust/cargo cpp/clang++
compile-lto thin 100000000 rust/cargo cpp/clang++
compile-lto full 100000000 rust/cargo cpp/clang++
startup-exe - 50000 cpp/g++ rust/rustc go/go java/java python/cpython
startup-cja-cold - 1000000 java/java python/cpython
startup-cja-warm - 1000000 java/java-cds python/cpython
binary-size-hello exe 50000 cpp/g++ rust/rustc go/go java/javac python/cpython
binary-size-tour exe 50000 cpp/g++ rust/cargo go/go java/javac python/cpython
build-noop - 1000000 java/gradle java/maven rust/cargo go/go cpp/ninja
kernel-startup - 100000000 python/ipykernel java/IJava rust/evcxr
debugger-attach - 50000000 java/jdb rust/gdb cpp/gdb go/delve python/debugpy
'
IDS="$(awk 'NF{print $1}' <<<"$REQUIRED" | sort -u | tr '\n' ' ')"

while IFS= read -r row; do
    [[ -z "$row" ]] && continue
    N="$(cawk '{print NF}' <<<"$row")"
    [[ "$N" == "$NCOLS" ]] || fail "row has $N cols, expected $NCOLS: $row"
    cawk '$1!="2" || $5!="toolchain" || $33=="" || $34=="" || $37=="" || $37!=$10 {exit 1}' <<<"$row" \
        || fail "row is not v2 toolchain/ with machine/os/platform: $row"
    b="$(cawk '{print $4}' <<<"$row")"
    case " $IDS " in *" $b "*) ;; *) fail "unexpected benchmark: $b" ;; esac
    st="$(cawk '{print $28}' <<<"$row")"
    if [[ "$st" == "skipped" ]]; then
        [[ -n "$(cawk '{print $30}' <<<"$row")" ]] || fail "skip row missing reason: $row"
    elif [[ "$st" != "ok" ]]; then
        fail "status '$st': $row"
    fi
done < <(csv_fields <<<"$OUT")

while read -r b v floor libs; do
    [[ -z "$b" ]] && continue
    [[ "$v" == "-" ]] && v=""
    for want in $libs; do
        lang="${want%%/*}"; lib="${want#*/}"
        r="$(cawk -v b="$b" -v v="$v" -v l="$lang" -v lib="$lib" '$4==b && $31==v && $10==l && $12==lib' <<<"$OUT" | head -1)"
        [[ -n "$r" ]] || fail "no $lang $lib row for toolchain/$b${v:+/$v}"
        [[ "$(cawk '{print $28 "/" $29}' <<<"$r")" == "ok/true" ]] \
            || fail "$lang $lib toolchain/$b${v:+/$v} not ok/check_ok: $(cawk '{print $28, $29, $30}' <<<"$r")"
        min="$(cawk '{print $17}' <<<"$r")"
        (( min >= floor )) || fail "$lang $lib toolchain/$b${v:+/$v} min_ns=$min under the $floor floor"
        if [[ "$b" == binary-size-* ]]; then
            kb="$(cawk '{print $23}' <<<"$r")"
            (( kb > 0 )) || fail "$lang $lib toolchain/$b size column is $kb"
            [[ "$(cawk '{print $30}' <<<"$r")" == *"size on disk"* ]] || fail "$lang $lib toolchain/$b does not say its memory column is the size on disk"
        fi
    done
    echo "[toolchain-comp] $b${v:+/$v}: $(cawk -v b="$b" -v v="$v" '$4==b && $31==v && $28=="ok" {printf "%s/%s ", $10, $12}' <<<"$OUT")"
done <<< "$REQUIRED"

echo "TOOLCHAIN-COMP OK"
