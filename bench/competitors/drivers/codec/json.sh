#!/usr/bin/env bash
# codec/json competitors (bench/codec/json/Json.md): the six json ids on the
# nativejson corpus and the JSON_checker set, one program per language under
# competitors/<lang>/codec/json/:
#   cpp     simdjson (DOM, On-Demand for bind-skip and the token walk) and yyjson
#   rust    serde_json (a counting Visitor for tokenize, IgnoredAny for bind-skip)
#   go      encoding/json (Decoder.Token, Unmarshal)
#   java    Jackson (JsonParser.nextToken, ObjectMapper)
#   python  json and orjson (no tokenizer or binder: those rows are skips)
# A missing toolchain or library is a skipped row per id with the reason.
# simdjson and yyjson come from competitors/third_party (setup.sh pins them at
# the vendor/*.lock commits), falling back to a vendor.sh clone.
#
# Env: PROFILE_DATA_DIR (the corpus + jsonchecker_*.json from
# datasets/fetch.sh), PROFILE_RUN_ID, PROFILE_RUN_TS, PROFILE_WARMUP (3),
# PROFILE_TRIALS (10), PROFILE_LANGS (space-list filter).
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
libdir() { if [[ -d "$DIR/third_party/$1" ]]; then echo "$DIR/third_party/$1"; else echo "$DIR/vendor/$1"; fi; }

IDS=""
for b in json-tokenize json-bind-skip json-dom json-serialize json-roundtrip; do
    for v in twitter citm_catalog canada; do IDS+=" $b:$v"; done
done
IDS+=" json-conformance:"

skip_lang() {   # lang lib reason
    local rid="${PROFILE_RUN_ID:-local}" ts="${PROFILE_RUN_TS:-}" e b v ds bytes
    for e in $IDS; do
        b="${e%%:*}"; v="${e#*:}"
        if [[ -n "$v" ]]; then ds="$v"; bytes=-1; [[ -f "$PROFILE_DATA_DIR/$v.json" ]] && bytes="$(stat -c%s "$PROFILE_DATA_DIR/$v.json")"
        else ds=jsonchecker; bytes=34; fi
        echo "1,${rid},${ts},${b},codec,${ds},${bytes},,${bytes},$1,,$2,,,0,0,-1,-1,-1,-1,,,-1,-1,-1,-1,-1,skipped,,$3,${v}"
    done
}

if want rust; then
    if command -v cargo >/dev/null 2>&1; then
        if ( cd "$DIR/rust/codec/json" && RUSTFLAGS="-C target-cpu=native" cargo build --release >"$LOGS/codec-json-rust.log" 2>&1 ); then
            PROFILE_LANG_VERSION="$(rustc --version | strip)" "$DIR/rust/codec/json/target/release/codec-json" \
                || skip_lang rust serde_json "rust json runner crashed"
        else skip_lang rust serde_json "cargo build failed (logs/codec-json-rust.log)"; fi
    else skip_lang rust serde_json "cargo not installed"; fi
fi

if want cpp; then
    SJ="$(libdir simdjson)"; YY="$(libdir yyjson)"
    if command -v g++ >/dev/null 2>&1 && [[ -f "$SJ/singleheader/simdjson.cpp" && -f "$YY/src/yyjson.c" ]]; then
        BIN="$BUILD/cpp-codec-json"; SRC="$DIR/cpp/codec/json/json.cpp"
        if [[ ! -x "$BIN" || "$SRC" -nt "$BIN" || "$DIR/cpp/codec/json/row.h" -nt "$BIN" ]]; then
            g++ -std=c++17 -O3 -march=native -DNDEBUG -I "$SJ/singleheader" -I "$YY/src" \
                "$SRC" "$SJ/singleheader/simdjson.cpp" "$YY/src/yyjson.c" -o "$BIN" >"$LOGS/codec-json-cpp.log" 2>&1 || rm -f "$BIN"
        fi
        if [[ -x "$BIN" ]]; then
            PROFILE_LANG_VERSION="g++$(g++ -dumpfullversion | cut -d. -f1)" "$BIN" || skip_lang cpp simdjson "cpp json runner crashed"
        else skip_lang cpp simdjson "g++ build failed (logs/codec-json-cpp.log)"; fi
    else skip_lang cpp simdjson "g++ or simdjson/yyjson missing (run competitors/setup.sh)"; fi
fi

if want go; then
    if command -v go >/dev/null 2>&1; then
        if ( cd "$DIR/go/codec/json" && go build -o "$BUILD/go-codec-json" . >"$LOGS/codec-json-go.log" 2>&1 ); then
            PROFILE_LANG_VERSION="$(go version | strip)" "$BUILD/go-codec-json" || skip_lang go encoding/json "go json runner failed"
        else skip_lang go encoding/json "go build failed (logs/codec-json-go.log)"; fi
    else skip_lang go encoding/json "go not installed"; fi
fi

if want python; then
    PY="$DIR/python/.venv/bin/python"; [[ -x "$PY" ]] || PY="$(command -v python3 || true)"
    if [[ -n "$PY" ]]; then
        PROFILE_LANG_VERSION="$("$PY" --version 2>&1 | strip)" "$PY" "$DIR/python/codec/json/json_bench.py" \
            || skip_lang python json "python json runner crashed"
    else skip_lang python json "python3 not installed"; fi
fi

if want java; then
    JBIN="$HOME/.sdkman/candidates/java/current/bin"; [[ -x "$JBIN/javac" ]] || JBIN="$(dirname "$(command -v javac 2>/dev/null || echo /nonexistent/javac)")"
    JM="$DIR/third_party/java/com/fasterxml/jackson/core"
    JCP="$JM/jackson-databind/2.22.0/jackson-databind-2.22.0.jar:$JM/jackson-core/2.22.0/jackson-core-2.22.0.jar:$JM/jackson-annotations/2.22/jackson-annotations-2.22.jar"
    if [[ -x "$JBIN/javac" && -f "$JM/jackson-databind/2.22.0/jackson-databind-2.22.0.jar" ]]; then
        OUT="$BUILD/java-codec-json"; mkdir -p "$OUT"
        if [[ ! -f "$OUT/CodecJson.class" || -n "$(find "$DIR/java/codec/json" -name '*.java' -newer "$OUT/CodecJson.class")" ]]; then
            "$JBIN/javac" -cp "$JCP" -d "$OUT" "$DIR"/java/codec/json/*.java >"$LOGS/codec-json-java.log" 2>&1 || rm -f "$OUT/CodecJson.class"
        fi
        if [[ -f "$OUT/CodecJson.class" ]]; then
            PROFILE_LANG_VERSION="$("$JBIN/java" -version 2>&1 | head -1 | strip)" "$JBIN/java" -cp "$OUT:$JCP" CodecJson \
                || skip_lang java jackson-databind "java json runner crashed"
        else skip_lang java jackson-databind "javac build failed (logs/codec-json-java.log)"; fi
    else skip_lang java jackson-databind "javac or the pinned Jackson jars missing (run competitors/setup.sh)"; fi
fi
