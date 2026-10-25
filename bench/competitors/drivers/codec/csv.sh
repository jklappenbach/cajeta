#!/usr/bin/env bash
# codec/csv-parse and codec/csv-write competitors (bench/codec/csv/Csv.md) on
# the generated file (datasets/gen_csv.py: 100 MB, or 4 MB with
# CAJETA_BENCH_SMOKE=1), one program per language under
# competitors/<lang>/codec/csv/:
#   cpp     fast-cpp-csv-parser (a reader only: csv-write is a skipped row)
#   rust    csv + serde (typed records)
#   go      encoding/csv + strconv
#   java    univocity-parsers
#   python  pandas read_csv/to_csv and the csv module
# The data file is generated here when it is missing. A missing toolchain or
# library is a skipped row per id with the reason.
# Env: PROFILE_DATA_DIR, CAJETA_BENCH_SMOKE, PROFILE_RUN_ID, PROFILE_RUN_TS,
# PROFILE_WARMUP (3), PROFILE_TRIALS (10), PROFILE_LANGS (space-list filter).
set -uo pipefail

DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )/../.." &> /dev/null && pwd )"   # competitors/
BENCH="$( cd -- "${DIR}/.." &> /dev/null && pwd )"
export PROFILE_DATA_DIR="${PROFILE_DATA_DIR:-${BENCH}/datasets/cache}"
BUILD="${DIR}/.build"; mkdir -p "$BUILD"
LOGS="${DIR}/logs"; mkdir -p "$LOGS"
LANGS="${PROFILE_LANGS:-rust cpp go python java}"
export PATH="$HOME/.cargo/bin:$PATH"
strip() { tr -d ',"'; }
want() { case " $LANGS " in *" $1 "*) return 0;; *) return 1;; esac; }
SCALE=full; case "${CAJETA_BENCH_SMOKE:-}" in 1|true|yes) SCALE=smoke ;; esac
CSV="$PROFILE_DATA_DIR/codec-csv-$SCALE.csv"
if [[ ! -f "$CSV" || ! -f "${CSV%.csv}.meta" ]]; then
    python3 "$BENCH/datasets/gen_csv.py" "$PROFILE_DATA_DIR" "$SCALE" >&2 || echo "csv.sh: gen_csv.py failed" >&2
fi

skip_ids() {   # lang lib reason ids...
    local lang="$1" lib="$2" reason="$3" rid="${PROFILE_RUN_ID:-local}" ts="${PROFILE_RUN_TS:-}" b bytes=-1; shift 3
    [[ -f "$CSV" ]] && bytes="$(stat -c%s "$CSV")"
    for b in "$@"; do
        echo "1,${rid},${ts},${b},codec,codec-csv-${SCALE},${bytes},,${bytes},${lang},,${lib},,,0,0,-1,-1,-1,-1,,,-1,-1,-1,-1,-1,skipped,,${reason},"
    done
}
ALL="csv-parse csv-write"

if want rust; then
    if command -v cargo >/dev/null 2>&1; then
        if ( cd "$DIR/rust/codec/csv" && RUSTFLAGS="-C target-cpu=native" cargo build --release >"$LOGS/codec-csv-rust.log" 2>&1 ); then
            PROFILE_LANG_VERSION="$(rustc --version | strip)" "$DIR/rust/codec/csv/target/release/codec-csv" \
                || skip_ids rust csv "rust csv runner crashed" $ALL
        else skip_ids rust csv "cargo build failed (logs/codec-csv-rust.log)" $ALL; fi
    else skip_ids rust csv "cargo not installed" $ALL; fi
fi

if want cpp; then
    FC="$DIR/third_party/fast-cpp-csv-parser"
    if command -v g++ >/dev/null 2>&1 && [[ -f "$FC/csv.h" ]]; then
        BIN="$BUILD/cpp-codec-csv"; SRC="$DIR/cpp/codec/csv/csv.cpp"
        if [[ ! -x "$BIN" || "$SRC" -nt "$BIN" || "$DIR/cpp/codec/csv/row.h" -nt "$BIN" ]]; then
            g++ -std=c++17 -O3 -march=native -DNDEBUG -I "$FC" "$SRC" -o "$BIN" >"$LOGS/codec-csv-cpp.log" 2>&1 || rm -f "$BIN"
        fi
        if [[ -x "$BIN" ]]; then
            PROFILE_LANG_VERSION="g++$(g++ -dumpfullversion | cut -d. -f1)" "$BIN" || skip_ids cpp fast-cpp-csv-parser "cpp csv runner crashed" $ALL
        else skip_ids cpp fast-cpp-csv-parser "g++ build failed (logs/codec-csv-cpp.log)" $ALL; fi
    else skip_ids cpp fast-cpp-csv-parser "g++ or the pinned fast-cpp-csv-parser checkout missing (run competitors/setup.sh)" $ALL; fi
fi

if want go; then
    if command -v go >/dev/null 2>&1; then
        if ( cd "$DIR/go/codec/csv" && go build -o "$BUILD/go-codec-csv" . >"$LOGS/codec-csv-go.log" 2>&1 ); then
            PROFILE_LANG_VERSION="$(go version | strip)" "$BUILD/go-codec-csv" || skip_ids go encoding/csv "go csv runner failed" $ALL
        else skip_ids go encoding/csv "go build failed (logs/codec-csv-go.log)" $ALL; fi
    else skip_ids go encoding/csv "go not installed" $ALL; fi
fi

if want python; then
    PY="$DIR/python/.venv/bin/python"; [[ -x "$PY" ]] || PY="$(command -v python3 || true)"
    if [[ -n "$PY" ]]; then
        PROFILE_LANG_VERSION="$("$PY" --version 2>&1 | strip)" "$PY" "$DIR/python/codec/csv/csv_bench.py" \
            || skip_ids python pandas "python csv runner crashed" $ALL
    else skip_ids python pandas "python3 not installed" $ALL; fi
fi

if want java; then
    JBIN="$HOME/.sdkman/candidates/java/current/bin"; [[ -x "$JBIN/javac" ]] || JBIN="$(dirname "$(command -v javac 2>/dev/null || echo /nonexistent/javac)")"
    UV="$DIR/third_party/java/com/univocity/univocity-parsers/2.9.1/univocity-parsers-2.9.1.jar"
    if [[ -x "$JBIN/javac" && -f "$UV" ]]; then
        OUT="$BUILD/java-codec-csv"; mkdir -p "$OUT"
        if [[ ! -f "$OUT/CodecCsv.class" || -n "$(find "$DIR/java/codec/csv" -name '*.java' -newer "$OUT/CodecCsv.class")" ]]; then
            "$JBIN/javac" -cp "$UV" -d "$OUT" "$DIR"/java/codec/csv/*.java >"$LOGS/codec-csv-java.log" 2>&1 || rm -f "$OUT/CodecCsv.class"
        fi
        if [[ -f "$OUT/CodecCsv.class" ]]; then
            PROFILE_LANG_VERSION="$("$JBIN/java" -version 2>&1 | head -1 | strip)" "$JBIN/java" -cp "$OUT:$UV" CodecCsv \
                || skip_ids java univocity-parsers "java csv runner crashed" $ALL
        else skip_ids java univocity-parsers "javac build failed (logs/codec-csv-java.log)" $ALL; fi
    else skip_ids java univocity-parsers "javac or the pinned univocity-parsers jar missing (run competitors/setup.sh)" $ALL; fi
fi
