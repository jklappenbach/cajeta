#!/usr/bin/env bash
# bench.sh driver checks that need no build and no run (benchmark-reference
# consolidation; spec §9.1). Every case runs `bench.sh --dry-run`, which does
# everything up to the build (argument parsing, PATH, the quiet-box check) and
# prints the plan as `plan: <key>=<value>` lines. The quiet-box inputs are
# stubbed: BENCH_LOADAVG_FILE stands in for /proc/loadavg, and stub `ps`,
# `nvidia-smi` and `rocm-smi` sit first on PATH, so nothing here reads a GPU.
# Asserts:
#  (1) the fork's bin is first on PATH (so --lto=thin finds ld.lld), and
#      ~/.cargo/bin is on it;
#  (2) --machine defaults to the hostname lowercased; --machine wins;
#  (3) --no-gpu exports CAJETA_BENCH_NO_GPU=1 and skips the GPU probe;
#  (4) a full-catalog run gates with --require-suite, a filtered one without;
#  (5) the dataset generators run before a run: fetch.sh, gen_dbbench.py,
#      gen_csv.py, gen_io.py and gen_wire.py on a full run; a filtered run only
#      its package's;
#  (6) a non-smoke run refuses (exit 3) on load >= 1, a CI runner job, GPU
#      utilization > 0 or a GPU compute process, naming the reason; --force
#      proceeds and says it was forced; a smoke run is never checked;
#  (7) the evidence is what env.csv will record (quiet_* lines).
set -uo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
PROJ="$( cd -- "${SCRIPT_DIR}/.." &> /dev/null && pwd )"
FAILS=0
fail() { echo "BENCH.SH FAIL: $1" >&2; FAILS=$((FAILS+1)); }

T="$(mktemp -d "${TMPDIR:-/tmp}/bench-sh-smoke.XXXXXX")"
trap 'rm -rf "$T"' EXIT
STUB="$T/stub"; mkdir -p "$STUB"

# stub_box <load> <gpu-util> <gpu-procs> <ci 0|1>
stub_box() {
    printf '%s 0.50 0.50 1/100 12345\n' "$1" > "$T/loadavg"
    cat > "$STUB/nvidia-smi" <<EOF
#!/bin/sh
case "\$*" in
  *query-gpu=utilization.gpu*) echo "$2" ;;
  *query-compute-apps*) [ "$3" -gt 0 ] && echo "4242, /usr/bin/python3" ; true ;;
  *) echo "stub nvidia-smi: unexpected \$*" >&2; exit 9 ;;
esac
EOF
    cat > "$STUB/ps" <<EOF
#!/bin/sh
echo bash
[ "$4" = 1 ] && echo Runner.Worker
echo sleep
EOF
    chmod +x "$STUB/nvidia-smi" "$STUB/ps"
}
# run bench.sh --dry-run with the stubs; extra env via `env` words first
dry() { env PATH="$STUB:$PATH" BENCH_LOADAVG_FILE="$T/loadavg" bash "$PROJ/bench.sh" --dry-run "$@" 2>&1; }
plan() { printf '%s\n' "$1" | sed -n "s/^plan: $2=//p" | head -1; }

# A quiet box.
stub_box 0.12 0 0 0
OUT="$(dry)"; RC=$?
[[ "$RC" == "0" ]] || fail "quiet box: --dry-run exit $RC: $OUT"

# (1) PATH
[[ "$(plan "$OUT" path_head)" == "/home/julian/code/cpp/cajeta-llvm/llvm-install/bin" || \
   "$(plan "$OUT" path_head)" == "$(cd "$PROJ/../../cajeta-llvm/llvm-install/bin" 2>/dev/null && pwd)" ]] \
    || fail "PATH head is '$(plan "$OUT" path_head)', want the fork's llvm-install/bin"
[[ "$(plan "$OUT" ld_lld)" == */cajeta-llvm/llvm-install/bin/ld.lld ]] || fail "ld.lld resolves to '$(plan "$OUT" ld_lld)', not the fork's"
[[ "$(plan "$OUT" cargo_bin_on_path)" == "yes" ]] || fail "~/.cargo/bin not on PATH"

# (2) machine
want="$(cat /proc/sys/kernel/hostname 2>/dev/null || hostname)"; want="$(printf '%s' "$want" | tr 'A-Z' 'a-z')"
[[ "$(plan "$OUT" machine)" == "$want" ]] || fail "default machine '$(plan "$OUT" machine)', want '$want'"
OUT2="$(env -u CAJETA_BENCH_MACHINE PATH="$STUB:$PATH" BENCH_LOADAVG_FILE="$T/loadavg" bash "$PROJ/bench.sh" --dry-run --machine Proton 2>&1)"
[[ "$(plan "$OUT2" machine)" == "proton" ]] || fail "--machine Proton gave '$(plan "$OUT2" machine)'"

# (3) --no-gpu
[[ "$(plan "$OUT" no_gpu)" == "0" ]] || fail "no_gpu set without --no-gpu"
OUT3="$(dry --no-gpu)"
[[ "$(plan "$OUT3" no_gpu)" == "1" ]] || fail "--no-gpu did not set CAJETA_BENCH_NO_GPU=1"
printf '%s\n' "$OUT3" | grep -q '^quiet_gpu_util,not probed' || fail "--no-gpu still probed the GPU: $OUT3"

# (4) gate
[[ "$(plan "$OUT" gate)" == "--require-suite" ]] || fail "full-catalog gate is '$(plan "$OUT" gate)'"
OUT4="$(dry --area collection)"
[[ "$(plan "$OUT4" gate)" == "" ]] || fail "filtered gate is '$(plan "$OUT4" gate)', want none"
OUT4b="$(dry --bench dot-product)"
[[ "$(plan "$OUT4b" gate)" == "" ]] || fail "--bench gate is '$(plan "$OUT4b" gate)', want none"

# (5) generators
[[ "$(plan "$OUT" datasets)" == "fetch gen_dbbench:1e6,1e7 gen_csv:full gen_io:full gen_wire" ]] \
    || fail "full-run datasets '$(plan "$OUT" datasets)'"
[[ "$(plan "$OUT4" datasets)" == "fetch" ]] || fail "collection datasets '$(plan "$OUT4" datasets)'"
[[ "$(plan "$(dry --area nucleo/table)" datasets)" == "fetch gen_dbbench:1e6,1e7" ]] || fail "nucleo datasets"
S="$(CAJETA_BENCH_SMOKE=1 dry --area codec)"
[[ "$(plan "$S" datasets)" == "fetch gen_csv:smoke" ]] || fail "smoke codec datasets '$(plan "$S" datasets)'"
[[ "$(plan "$(CAJETA_BENCH_SMOKE=1 dry)" datasets)" == "fetch gen_dbbench:1e5 gen_csv:smoke gen_io:smoke gen_wire" ]] || fail "smoke full datasets"

# (6) quiet box
[[ "$(plan "$OUT" quiet)" == "pass" ]] || fail "quiet box not pass: $(plan "$OUT" quiet)"
check_refuse() {  # label pattern
    local o rc; o="$(dry)"; rc=$?
    [[ "$rc" == "3" ]] || fail "$1: exit $rc, want 3"
    printf '%s\n' "$o" | grep -qi "$2" || fail "$1: refusal does not name '$2': $o"
    printf '%s\n' "$o" | grep -q "^quiet_check,refused" || fail "$1: refusal evidence is not quiet_check,refused"
    o="$(dry --force)"; rc=$?
    [[ "$rc" == "0" ]] || fail "$1 --force: exit $rc"
    [[ "$(plan "$o" quiet)" == forced* ]] || fail "$1 --force: quiet '$(plan "$o" quiet)'"
    printf '%s\n' "$o" | grep -q '^quiet_check,forced' || fail "$1 --force: no quiet_check,forced evidence"
}
stub_box 1.50 0 0 0; check_refuse "load 1.50" "load average 1.50"
stub_box 0.10 0 0 1; check_refuse "CI job" "CI runner"
stub_box 0.10 37 0 0; check_refuse "GPU busy" "GPU utilization 37"
stub_box 0.10 0 1 0; check_refuse "GPU process" "compute process"
stub_box 1.50 37 0 0
S="$(CAJETA_BENCH_SMOKE=1 dry)"; RC=$?
[[ "$RC" == "0" && "$(plan "$S" quiet)" == skipped* ]] || fail "smoke run was quiet-checked (exit $RC, $(plan "$S" quiet))"

# (7) evidence lines
stub_box 0.12 0 0 0; OUT="$(dry)"
for k in quiet_load_avg quiet_ci_jobs quiet_gpu_util quiet_gpu_procs quiet_check; do
    printf '%s\n' "$OUT" | grep -q "^$k," || fail "no $k evidence line"
done
printf '%s\n' "$OUT" | grep -qx 'quiet_load_avg,0.12' || fail "load evidence wrong"
printf '%s\n' "$OUT" | grep -qx 'quiet_check,pass' || fail "check evidence wrong"

if [[ "$FAILS" -gt 0 ]]; then echo "BENCH.SH FAILED ($FAILS)"; exit 1; fi
echo "BENCH.SH OK"
