#!/usr/bin/env bash
# time/ competitor program `datetime` (benchmark-reference unit 8): every id of
# bench/time/*.md in every language the pages name, same sizes, inputs and
# check constants as bench/src/main/cajeta/bench/time/:
#   clock-now, time-instant-arith, time-localdate-arith, duration-arith,
#   localtime-arith, localdatetime-arith (1e7 each),
#   zoneddatetime-region (1e6, America/Los_Angeles), datetime-format-iso and
#   datetime-parse-iso (1e6).
# Libraries: cpp std::chrono (libstdc++ zoned_time, std::format) + Howard
# Hinnant's date for parse (third_party/date, pins.tsv); rust jiff + chrono
# (+ std Instant for the clock); go time; java java.time; python datetime,
# zoneinfo, time and whenever. A missing toolchain or library is a `skipped`
# row per id with the reason, never a crash. Rows are v1; run.sh normalizes.
#
# Env: PROFILE_RUN_ID, PROFILE_RUN_TS, PROFILE_WARMUP (3), PROFILE_TRIALS (10),
#      PROFILE_LANGS (space-list filter).
set -uo pipefail

DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )/../.." &> /dev/null && pwd )"   # competitors/
BUILD="${DIR}/.build"; mkdir -p "$BUILD"
LOGS="${BUILD}/logs"; mkdir -p "$LOGS"
TP="${DIR}/third_party"
LANGS="${PROFILE_LANGS:-cpp rust go java python}"
strip() { tr -d ',"'; }
want() { case " $LANGS " in *" $1 "*) return 0;; *) return 1;; esac; }

# skip <lang> <reason> <bench:size:lib>...
skip() {
    local lang="$1" reason="$2" rid="${PROFILE_RUN_ID:-local}" ts="${PROFILE_RUN_TS:-}" spec b sz lib
    shift 2
    for spec in "$@"; do
        IFS=: read -r b sz lib <<<"$spec"
        echo "1,${rid},${ts},${b},time,,${sz},,${sz},${lang},,${lib},,,0,0,-1,-1,-1,-1,,,-1,-1,-1,-1,-1,skipped,false,${reason},"
    done
}
ALL9="clock-now:10000000 time-instant-arith:10000000 time-localdate-arith:10000000 duration-arith:10000000 localtime-arith:10000000 localdatetime-arith:10000000 zoneddatetime-region:1000000 datetime-format-iso:1000000 datetime-parse-iso:1000000"
specs() { local lib="$1" s; shift; for s in "$@"; do echo "$s:$lib"; done; }

# ---- cpp: std::chrono (g++ 13) + date for parse ----
if want cpp; then
    S="$DIR/cpp/time/datetime/datetime_bench.cpp"; BIN="$BUILD/cpp-datetime"
    CPP_SPECS=$(specs std::chrono $ALL9 | sed 's/datetime-parse-iso:1000000:std::chrono/datetime-parse-iso:1000000:date/')
    if ! command -v g++ >/dev/null 2>&1; then
        skip cpp "g++ not installed" $CPP_SPECS
    elif [[ ! -f "$TP/date/include/date/date.h" ]]; then
        skip cpp "third_party/date missing (run competitors/setup.sh)" $CPP_SPECS
    else
        if [[ ! -x "$BIN" || "$S" -nt "$BIN" ]]; then
            g++ -O3 -march=native -std=c++20 -DNDEBUG -I "$TP/date/include" "$S" -o "$BIN" >"$LOGS/cpp-datetime.log" 2>&1 || rm -f "$BIN"
        fi
        if [[ -x "$BIN" ]]; then
            PROFILE_LANG_VERSION="$(g++ --version | head -1 | strip)" "$BIN" || skip cpp "cpp datetime runner crashed" $CPP_SPECS
        else
            skip cpp "g++ build failed (see .build/logs/cpp-datetime.log)" $CPP_SPECS
        fi
    fi
fi

# ---- rust: jiff + chrono ----
if want rust; then
    RS_SPECS="clock-now:10000000:std $(specs jiff ${ALL9#clock-now:10000000 }) $(specs chrono time-instant-arith:10000000 time-localdate-arith:10000000 duration-arith:10000000 localtime-arith:10000000 localdatetime-arith:10000000 datetime-format-iso:1000000 datetime-parse-iso:1000000)"
    if ! command -v cargo >/dev/null 2>&1; then
        skip rust "cargo not installed" $RS_SPECS
    elif ( cd "$DIR/rust/time/datetime" && cargo build --release >"$LOGS/rust-datetime.log" 2>&1 ); then
        PROFILE_LANG_VERSION="$(rustc --version | strip)" "$DIR/rust/time/datetime/target/release/bench-datetime" \
            || skip rust "rust datetime runner crashed" $RS_SPECS
    else
        skip rust "cargo build failed (see .build/logs/rust-datetime.log)" $RS_SPECS
    fi
fi

# ---- go: time ----
if want go; then
    if command -v go >/dev/null 2>&1; then
        ( cd "$DIR/go/time/datetime" && GOAMD64="${GOAMD64:-v3}" PROFILE_LANG_VERSION="$(go version | strip)" go run . 2>"$LOGS/go-datetime.log" ) \
            || skip go "go build/run failed (see .build/logs/go-datetime.log)" $(specs time $ALL9)
    else
        skip go "go not installed" $(specs time $ALL9)
    fi
fi

# ---- java: java.time ----
if want java; then
    if command -v javac >/dev/null 2>&1; then
        OUT="$BUILD/java-datetime"; SRC="$DIR/java/time/datetime/DateTimeBench.java"
        if [[ ! -f "$OUT/DateTimeBench.class" || "$SRC" -nt "$OUT/DateTimeBench.class" ]]; then
            mkdir -p "$OUT"; javac -d "$OUT" "$SRC" >"$LOGS/java-datetime.log" 2>&1 || rm -f "$OUT/DateTimeBench.class"
        fi
        if [[ -f "$OUT/DateTimeBench.class" ]]; then
            PROFILE_LANG_VERSION="$(java -version 2>&1 | head -1 | strip)" java -cp "$OUT" DateTimeBench \
                || skip java "java datetime runner crashed" $(specs java.time $ALL9)
        else
            skip java "javac build failed (see .build/logs/java-datetime.log)" $(specs java.time $ALL9)
        fi
    else
        skip java "javac not installed" $(specs java.time $ALL9)
    fi
fi

# ---- python: datetime + zoneinfo + time + whenever ----
if want python; then
    PY="$DIR/python/.venv/bin/python"
    PY_SPECS="clock-now:10000000:time zoneddatetime-region:1000000:zoneinfo $(specs datetime time-instant-arith:10000000 time-localdate-arith:10000000 duration-arith:10000000 localtime-arith:10000000 localdatetime-arith:10000000 datetime-format-iso:1000000 datetime-parse-iso:1000000) $(specs whenever time-instant-arith:10000000 time-localdate-arith:10000000 duration-arith:10000000 localtime-arith:10000000 localdatetime-arith:10000000 datetime-format-iso:1000000 datetime-parse-iso:1000000)"
    if [[ ! -x "$PY" ]]; then
        skip python "competitors/python/.venv missing (run competitors/setup.sh)" $PY_SPECS
    elif ! "$PY" -c "import whenever" >/dev/null 2>&1; then
        skip python "whenever not installed in the venv (run competitors/setup.sh)" $PY_SPECS
    else
        PROFILE_LANG_VERSION="$("$PY" --version 2>&1 | strip)" "$PY" "$DIR/python/time/datetime/datetime_bench.py" \
            || skip python "python datetime runner crashed" $PY_SPECS
    fi
fi
