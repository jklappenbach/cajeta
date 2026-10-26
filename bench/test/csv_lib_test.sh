#!/usr/bin/env bash
# Unit test of test/csv_lib.sh: quoted fields split right, rows printed by one
# check feed the next, several operands keep NR==FNR, stdin works.
set -uo pipefail
SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
source "$SCRIPT_DIR/csv_lib.sh"   # cawk / csv_fields: CSV-aware column checks
source "$SCRIPT_DIR/csv_lib.sh"
FAILS=0; fail() { echo "CSV_LIB FAIL: $1" >&2; FAILS=$((FAILS+1)); }
T="$(mktemp -d "${TMPDIR:-/tmp}/csv-lib-test.XXXXXX")"; trap 'rm -rf "$T"' EXIT
printf 'a,b,c\n1,"x, y",3\n2,"say ""hi"", ok",4\n' > "$T/t.csv"
[[ "$(cawk 'NR>1 {print $2 "|" $3}' "$T/t.csv" | tr '\n' ';')" == 'x, y|3;say "hi", ok|4;' ]] || fail "quoted fields"
row="$(cawk 'NR==2' "$T/t.csv")"
[[ "$(cawk '{print NF "|" $3}' <<<"$row")" == "3|3" ]] || fail "printed row did not pass through"
IFS="$CSV_FS" read -r -a c <<<"$row"; [[ "${c[1]}" == "x, y" ]] || fail "read -a on a printed row: ${c[1]}"
[[ "$(cawk -v x=1 'NR==FNR {n++; next} END {print n "|" FNR}' "$T/t.csv" "$T/t.csv")" == "3|3" ]] || fail "two operands"
[[ "$(printf '2,run,,"multi\nline",x\n' | cawk '{print NF "|" $4}')" == "5|multi line" ]] || fail "stdin + embedded newline"
[[ "$(cawk -F, '{print $2; exit}' "$T/t.csv")" == "b" ]] || fail "-F, not ignored"
[[ "$(csv_fields "$T/t.csv" | wc -l)" == "3" ]] || fail "csv_fields line count"
[[ "$(cawk 'END {print NR}' "$T/t.csv")" == "3" ]] || fail "file operand"
# An early-exiting reader is not a pipeline failure under pipefail.
python3 -c 'import sys; [print("2,x,%d,\"a, b\"" % i) for i in range(200000)]' > "$T/big.csv"
( set -o pipefail; cawk '{print; exit}' "$T/big.csv" >/dev/null 2>"$T/err"; rc=$?; [[ "$rc" == 0 && ! -s "$T/err" ]] ) || fail "cawk early exit fails the pipeline: $(head -1 "$T/err")"
( set -o pipefail; cawk 'NR>1' "$T/big.csv" | grep -q . ) || fail "cawk | grep -q fails the pipeline (awk SIGPIPE)"
( set -o pipefail; cawk 'NR>1' < "$T/big.csv" | head -1 >/dev/null ) || fail "stdin cawk | head fails the pipeline"
( set -o pipefail; ! cawk 'NR>1' "$T/missing.csv" 2>/dev/null | grep -q . ) || fail "a missing file reads as success"
if [[ "$FAILS" -gt 0 ]]; then echo "CSV_LIB FAILED ($FAILS)"; exit 1; fi
echo "CSV_LIB OK"
