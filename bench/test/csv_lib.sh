# CSV-aware column checks for the smokes (sourced). dev.cajeta.bench quotes
# free-text fields (RFC 4180), so the smokes never split a row on commas:
#
#   cawk [awk options] 'program' [file ...]   awk over the csv module's parse
#                         (test/csv_fields.py): fields are split correctly, $N
#                         is column N, and a printed row ($0) is joined by
#                         CSV_FS, which cawk and csv_fields pass through.
#   csv_fields [file]     the rows in that form, for `while read` loops.
#   IFS="$CSV_FS" read -r -a c <<<"$row"    split one such row.
#
# -F is ignored (the separator is the csv module's). OFS defaults to CSV_FS.
CSV_FS=$'\x1f'
CSV_LIB_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
csv_fields() { python3 "$CSV_LIB_DIR/csv_fields.py" "$@"; }
cawk() {
    local opts=() prog="" have_prog=0 files=() f tmp rc st
    while [[ $# -gt 0 ]]; do
        if [[ "$have_prog" == "0" ]]; then
            case "$1" in
                -v|-f) opts+=("$1" "$2"); shift 2 ;;
                -F) shift 2 ;;
                -F*) shift ;;
                --) shift ;;
                -*) opts+=("$1"); shift ;;
                *) prog="$1"; have_prog=1; shift ;;
            esac
        else
            files+=("$1"); shift
        fi
    done
    # A reader that stops early (| grep -q, | head -1) closes the pipe; awk's
    # SIGPIPE (141) is not a failed check, so it reads as 0, the way a plain
    # awk over a file that wrote its output in one block never saw it.
    if [[ "${#files[@]}" == "0" ]]; then
        csv_fields | awk -F"$CSV_FS" -v OFS="$CSV_FS" ${opts[@]+"${opts[@]}"} "$prog"
        st=("${PIPESTATUS[@]}")
        [[ "${st[0]}" == 0 ]] || return "${st[0]}"
        [[ "${st[1]}" == 141 ]] && return 0; return "${st[1]}"
    elif [[ "${#files[@]}" == "1" && "${files[0]}" != *=* ]]; then
        csv_fields "${files[0]}" | awk -F"$CSV_FS" -v OFS="$CSV_FS" ${opts[@]+"${opts[@]}"} "$prog"
        st=("${PIPESTATUS[@]}")
        [[ "${st[0]}" == 0 ]] || return "${st[0]}"
        [[ "${st[1]}" == 141 ]] && return 0; return "${st[1]}"
    else
        # several operands: one parsed copy each, so NR==FNR idioms still work
        tmp="$(mktemp -d "${TMPDIR:-/tmp}/cawk.XXXXXX")"; local args=() i=0
        for f in "${files[@]}"; do
            if [[ "$f" == *=* && ! -e "$f" ]]; then args+=("$f"); continue; fi
            i=$((i+1)); csv_fields "$f" > "$tmp/$i"; args+=("$tmp/$i")
        done
        awk -F"$CSV_FS" -v OFS="$CSV_FS" ${opts[@]+"${opts[@]}"} "$prog" "${args[@]}"; rc=$?
        rm -rf "$tmp"; [[ "$rc" == 141 ]] && rc=0; return "$rc"
    fi
}
