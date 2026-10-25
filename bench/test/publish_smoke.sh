#!/usr/bin/env bash
# bench.sh publish on fixtures (benchmark-reference plan 22.1.1, spec §9.4).
# Builds a throwaway compiler-repo run and four ecosystem repos' runs in a temp
# tree, points publish at them (BENCH_PUBLISH_SRC, BENCH_PUBLISH_REPOS,
# BENCH_PUBLISH_DEST), and asserts:
#  (1) every source's rows are collected, ids unchanged (no area prefix: the
#      area already is the package), smoke rows dropped;
#  (2) env.csv is every source's env.csv merged under a key,value,source
#      header, quoting kept;
#  (3) verify-fidelity runs with --require-suite and --no-smoke, and LATEST
#      names the run; the site regen command is printed;
#  (4) a repo with no rows for the run is reported absent, not fatal;
#  (5) refusals, each leaving no run directory and no LATEST change: a row
#      from another machine, an ecosystem row outside its repo's package, the
#      same id from two sources, a v1 header, a failing fidelity gate, an
#      existing run directory, a missing compiler run;
#  (6) usage errors exit 2.
# Writes nothing under bench/results/.
set -uo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
source "$SCRIPT_DIR/csv_lib.sh"
PROJ="$( cd -- "${SCRIPT_DIR}/.." &> /dev/null && pwd )"
FAILS=0
fail() { echo "PUBLISH FAIL: $1" >&2; FAILS=$((FAILS+1)); }

T="$(mktemp -d "${TMPDIR:-/tmp}/publish-smoke.XXXXXX")"
trap 'rm -rf "$T"' EXIT
M=fixturebox
RUN=20261101T0300Z-0.46.3
H="schema_version,run_id,timestamp_unix,benchmark,area,dataset_name,dataset_bytes,dataset_sha256,input_size,language,language_version,library,library_version,flags,warmup,trials,min_ns,median_ns,mean_ns,p95_ns,throughput,throughput_unit,peak_rss_kb,cgroup_peak_kb,alloc_bytes,alloc_count,working_set_kb,status,check_ok,notes,variant,backend,machine,os,driver,model,platform"
# row <bench> <area> <language> <notes> [input_size] [machine] [platform]
row() {
    local b="$1" a="$2" l="$3" n="$4" in="${5:--1}" m="${6:-$M}" p="${7:-$3}"
    echo "2,$RUN,1790000000,$b,$a,,-1,,$in,$l,,,,,20,200,1000,1100,1150,1300,,,4096,-1,800,12,0,ok,true,$n,,cpu,$m,Ubuntu 24.04 (WSL2),,,$p"
}
mkrun() {  # dir rows...
    local d="$1"; shift; mkdir -p "$d"
    { echo "$H"; printf '%s\n' "$@"; } > "$d/results.csv"
}
COMPILER_ROWS=(
    "$(row string-build-concat lang cajeta "" 4000)"
    "$(row string-build-concat lang rust "" 4000)"
    "$(row json-dom codec cajeta "")"
    "$(row json-serialize codec cajeta "")"
    "$(row json-roundtrip codec cajeta "")"
    "$(row hashmap-int collection cajeta '"external-free; a note, with a comma"')"
    "$(row hashmap-int collection cajeta smoke)"
)
setup() {
    rm -rf "$T/src" "$T/repos" "$T/dest"; mkdir -p "$T/dest"
    mkrun "$T/src" "${COMPILER_ROWS[@]}"
    printf 'key,value\nmachine,%s\ncpu,"AMD Ryzen 9 3950X, 16 cores"\nquiet_check,pass\n' "$M" > "$T/src/env.csv"
    mkrun "$T/repos/cajeta-llm/bench/results/$M/$RUN" "$(row decode llm cajeta-llm "" -1 "$M" "cajeta-llm 0.9.0 cuda")" "$(row decode llm cajeta-llm smoke)"
    printf 'key,value\nmodel,Llama-3.1-8B Q4_K_M\n' > "$T/repos/cajeta-llm/bench/results/$M/$RUN/env.csv"
    mkrun "$T/repos/cajeta-ml/bench/results/$M/$RUN" "$(row knn-fit ml cajeta "")"
    mkrun "$T/repos/cajeta-xgboost/bench/results/$M/$RUN" "$(row xgb-fit ml cajeta "")"
    mkrun "$T/repos/cajeta-codec/bench/results/$M/$RUN" "$(row deflate-oneshot codec cajeta "")"
    mkdir -p "$T/repos/cajeta-http"   # no run: absent
}
repos() { echo "cajeta-llm=$T/repos/cajeta-llm cajeta-ml=$T/repos/cajeta-ml cajeta-xgboost=$T/repos/cajeta-xgboost cajeta-http=$T/repos/cajeta-http cajeta-codec=$T/repos/cajeta-codec"; }
pub() { env BENCH_PUBLISH_SRC="$T/src" BENCH_PUBLISH_REPOS="$(repos)" BENCH_PUBLISH_DEST="$T/dest" bash "$PROJ/bench.sh" publish "$@" 2>&1; }
OUTD="$T/dest/$M/$RUN"

# (1)-(4) the happy path
setup
OUT="$(pub "$M" "$RUN")"; RC=$?
[[ "$RC" == "0" ]] || fail "publish exit $RC: $OUT"
[[ -f "$OUTD/results.csv" && -f "$OUTD/env.csv" ]] || fail "no results.csv/env.csv under dest/$M/$RUN"
[[ "$(head -1 "$OUTD/results.csv")" == "$H" ]] || fail "header is not schema v2"
IDS="$(cawk 'NR>1 {print $5 "/" $4 "|" $10}' "$OUTD/results.csv" | sort | tr '\n' ' ')"
WANT="codec/deflate-oneshot|cajeta codec/json-dom|cajeta codec/json-roundtrip|cajeta codec/json-serialize|cajeta collection/hashmap-int|cajeta lang/string-build-concat|cajeta lang/string-build-concat|rust llm/decode|cajeta-llm ml/knn-fit|cajeta ml/xgb-fit|cajeta "
[[ "$IDS" == "$WANT" ]] || fail "rows: [$IDS] want [$WANT]"
[[ "$(cawk 'NR>1 && $30 ~ /(^|;)smoke(;|$)/' "$OUTD/results.csv" | wc -l)" == "0" ]] || fail "a smoke row was published"
[[ "$(cawk 'NR>1 && $4=="hashmap-int" {print $30}' "$OUTD/results.csv")" == "external-free; a note, with a comma" ]] || fail "quoted notes did not survive"
[[ "$(head -1 "$OUTD/env.csv")" == "key,value,source" ]] || fail "env.csv header: $(head -1 "$OUTD/env.csv")"
grep -qx 'cpu,"AMD Ryzen 9 3950X, 16 cores",cajeta' "$OUTD/env.csv" || fail "compiler env line missing or unquoted: $(cat "$OUTD/env.csv")"
grep -qx 'model,Llama-3.1-8B Q4_K_M,cajeta-llm' "$OUTD/env.csv" || fail "llm env line missing"
[[ "$(cat "$T/dest/$M/LATEST" 2>/dev/null)" == "$RUN" ]] || fail "LATEST is '$(cat "$T/dest/$M/LATEST" 2>/dev/null)'"
printf '%s\n' "$OUT" | grep -q -- '--require-suite --no-smoke' || fail "verify-fidelity was not run with --require-suite --no-smoke: $OUT"
printf '%s\n' "$OUT" | grep -q 'npm run regen' || fail "no site regen command printed"
printf '%s\n' "$OUT" | grep -q 'cajeta-http: absent' || fail "the absent repo is not reported: $OUT"
printf '%s\n' "$OUT" | grep -q '2 smoke rows dropped' || fail "smoke count not reported: $OUT"

# (5) refusals
refuse() {  # label pattern
    local o rc; o="$(pub "$M" "$RUN")"; rc=$?
    [[ "$rc" == "1" ]] || fail "$1: exit $rc, want 1 ($o)"
    printf '%s\n' "$o" | grep -qi -- "$2" || fail "$1: message does not say '$2': $o"
    [[ ! -e "$OUTD" ]] || fail "$1: wrote dest/$M/$RUN"
    [[ ! -e "$T/dest/$M/LATEST" ]] || fail "$1: wrote LATEST"
}
setup; mkrun "$T/repos/cajeta-ml/bench/results/$M/$RUN" "$(row knn-fit ml cajeta "" -1 proton)"; refuse "other machine" "machine proton"
setup; mkrun "$T/repos/cajeta-ml/bench/results/$M/$RUN" "$(row decode llm cajeta "")"; refuse "foreign package" "llm/decode is not cajeta-ml's"
setup; mkrun "$T/repos/cajeta-codec/bench/results/$M/$RUN" "$(row json-dom codec cajeta "")"; refuse "codec non-ecosystem id" "codec/json-dom is not cajeta-codec's"
setup; mkrun "$T/repos/cajeta-xgboost/bench/results/$M/$RUN" "$(row knn-fit ml cajeta "")"; refuse "duplicate id" "knn-fit.*twice\|twice.*knn-fit"
setup; printf '%s\n' "${H%,machine,os,driver,model,platform}" "1,r,0,x,ml,,-1,,-1,cajeta,,,,,1,1,1,1,1,1,,,1,-1,0,-1,0,ok,true,,,cpu" > "$T/repos/cajeta-ml/bench/results/$M/$RUN/results.csv"; refuse "v1 header" "schema v2"
setup; mkrun "$T/src" "$(row json-dom codec cajeta "")"; refuse "fidelity gate" "verify-fidelity"
setup; rm -rf "$T/src"; refuse "no compiler run" "no compiler-repo run"
setup; mkdir -p "$OUTD"; o="$(pub "$M" "$RUN")"; rc=$?
[[ "$rc" == "1" ]] && printf '%s\n' "$o" | grep -q 'already published' || fail "existing run dir: exit $rc ($o)"
[[ ! -e "$T/dest/$M/LATEST" ]] || fail "existing run dir: wrote LATEST"

# (6) usage
pub >/dev/null; [[ "$?" == "2" ]] || fail "no arguments: want exit 2"
pub "$M" >/dev/null; [[ "$?" == "2" ]] || fail "one argument: want exit 2"
pub "$M" "not a run id" >/dev/null; [[ "$?" == "2" ]] || fail "bad run id: want exit 2"

# Nothing landed in the real results tree.
[[ ! -e "$PROJ/results/$M" ]] || fail "wrote under bench/results/$M"

if [[ "$FAILS" -gt 0 ]]; then echo "PUBLISH FAILED ($FAILS)"; exit 1; fi
echo "PUBLISH OK"
