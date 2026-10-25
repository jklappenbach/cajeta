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
[[ "$(ccawk '{print $2; exit}' "$T/t.csv")" == "b" ]] || fail "-F, not ignored"
[[ "$(csv_fields "$T/t.csv" | wc -l)" == "3" ]] || fail "csv_fields line count"
[[ "$(cawk 'END {print NR}' "$T/t.csv")" == "3" ]] || fail "file operand"
if [[ "$FAILS" -gt 0 ]]; then echo "CSV_LIB FAILED ($FAILS)"; exit 1; fi
echo "CSV_LIB OK"
