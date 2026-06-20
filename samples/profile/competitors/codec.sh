#!/usr/bin/env bash
# Codec (JSON) competitor area runner. Builds + runs each present language's
# parse-to-DOM runner against the nativejson corpus and streams schema-conformant
# CSV rows (competitors/columns.txt order) to stdout — the same `json-dom`
# benchmark the Cajeta side emits, so they line up in the report's codec table.
#
# A missing toolchain or library never aborts the matrix: it is recorded as a
# `skip` row with a reason. Languages covered: rust (serde_json, simd-json),
# cpp (simdjson, yyjson), go (encoding/json, goccy/go-json), python (json,
# orjson, msgspec, ujson). Java needs Maven/JSON jars (no stdlib JSON) and is
# recorded as a skip until provisioned.
#
# Env: PROFILE_DATA_DIR (corpus dir, required), PROFILE_RUN_ID, PROFILE_RUN_TS,
#      PROFILE_WARMUP (3), PROFILE_TRIALS (10), PROFILE_LANGS (space list filter).
set -uo pipefail

DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
PROFILE="$( cd -- "${DIR}/.." &> /dev/null && pwd )"
DATA_DIR="${PROFILE_DATA_DIR:-${PROFILE}/datasets/cache}"
BUILD="${DIR}/.build"; mkdir -p "$BUILD"
VENDOR="${DIR}/vendor"
LANGS="${PROFILE_LANGS:-rust cpp go python java}"
export PROFILE_DATA_DIR="$DATA_DIR"
strip() { tr -d ',"'; }

CORPUS=("twitter twitter.json" "citm_catalog citm_catalog.json" "canada canada.json")

# Emit one skip row per corpus file for a language that can't run.
skip_lang() {
    local lang="$1" lib="$2" reason="$3"
    local rid="${PROFILE_RUN_ID:-local}" ts="${PROFILE_RUN_TS:-}"
    local entry name file bytes
    for entry in "${CORPUS[@]}"; do
        name="${entry%% *}"; file="${entry##* }"
        bytes=-1; [[ -f "$DATA_DIR/$file" ]] && bytes="$(stat -c%s "$DATA_DIR/$file")"
        echo "1,${rid},${ts},json-dom,codec,${name},${bytes},,${bytes},${lang},,${lib},,,0,0,-1,-1,-1,-1,,,-1,-1,-1,-1,-1,skipped,,${reason},"
    done
}

want() { case " $LANGS " in *" $1 "*) return 0;; *) return 1;; esac; }

# ---- rust: serde_json + simd-json (cargo) ----
if want rust; then
    if command -v cargo >/dev/null 2>&1; then
        if ( cd "$DIR/rust/codec" && cargo build --release >/tmp/profile-codec-rust.log 2>&1 ); then
            PROFILE_LANG_VERSION="$(rustc --version | strip)" "$DIR/rust/codec/target/release/profile-codec" \
                || skip_lang rust rust "rust codec runner crashed"
        else
            skip_lang rust rust "cargo build failed (see /tmp/profile-codec-rust.log)"
        fi
    else
        skip_lang rust rust "cargo not installed"
    fi
fi

# ---- cpp: simdjson + yyjson (vendored single-file amalgamations) ----
if want cpp; then
    if command -v g++ >/dev/null 2>&1 && [[ -f "$VENDOR/simdjson/singleheader/simdjson.cpp" && -f "$VENDOR/yyjson/src/yyjson.c" ]]; then
        BIN="$BUILD/cpp-codec"
        if [[ ! -x "$BIN" || "$DIR/cpp/codec/json_dom.cpp" -nt "$BIN" ]]; then
            g++ -O3 -march=native -DNDEBUG \
                -I "$VENDOR/simdjson/singleheader" -I "$VENDOR/yyjson/src" \
                "$DIR/cpp/codec/json_dom.cpp" \
                "$VENDOR/simdjson/singleheader/simdjson.cpp" \
                "$VENDOR/yyjson/src/yyjson.c" \
                -o "$BIN" >/tmp/profile-codec-cpp.log 2>&1 || true
        fi
        if [[ -x "$BIN" ]]; then
            PROFILE_LANG_VERSION="$(g++ --version | head -1 | strip)" "$BIN" \
                || skip_lang cpp cpp "cpp codec runner crashed"
        else
            skip_lang cpp cpp "g++ build failed (see /tmp/profile-codec-cpp.log)"
        fi
    else
        skip_lang cpp cpp "g++ or vendored simdjson/yyjson missing (run vendor.sh simdjson yyjson)"
    fi
fi

# ---- go: encoding/json + goccy/go-json ----
if want go; then
    if command -v go >/dev/null 2>&1; then
        if ( cd "$DIR/go/codec" && go mod tidy >/tmp/profile-codec-go.log 2>&1 && \
             PROFILE_LANG_VERSION="$(go version | strip)" go run . 2>>/tmp/profile-codec-go.log ); then
            :
        else
            skip_lang go go "go build/run failed (see /tmp/profile-codec-go.log)"
        fi
    else
        skip_lang go go "go not installed"
    fi
fi

# ---- python: json + orjson + msgspec + ujson (per-lib skip handled in runner) ----
if want python; then
    PY="$DIR/python/.venv/bin/python"
    [[ -x "$PY" ]] || PY="$(command -v python3 || true)"
    if [[ -n "$PY" ]]; then
        PROFILE_LANG_VERSION="$("$PY" --version 2>&1 | strip)" "$PY" "$DIR/python/codec/json_dom.py" \
            || skip_lang python python "python codec runner crashed"
    else
        skip_lang python python "python3 not installed"
    fi
fi

# ---- java: jackson-databind DOM (JDK has no stdlib JSON; jars vendored) ----
if want java; then
    JJARS="$VENDOR/jackson"
    JCP="$JJARS/jackson-databind-2.22.0.jar:$JJARS/jackson-core-2.22.0.jar:$JJARS/jackson-annotations-2.22.jar"
    # Fetch the jars on demand (recorded in vendor/jackson.lock) if missing + network is up.
    if [[ ! -f "$JJARS/jackson-databind-2.22.0.jar" ]] && command -v curl >/dev/null 2>&1; then
        mkdir -p "$JJARS"; B=https://repo1.maven.org/maven2/com/fasterxml/jackson/core
        curl -sSfL -o "$JJARS/jackson-core-2.22.0.jar"        "$B/jackson-core/2.22.0/jackson-core-2.22.0.jar"               2>/dev/null || true
        curl -sSfL -o "$JJARS/jackson-databind-2.22.0.jar"    "$B/jackson-databind/2.22.0/jackson-databind-2.22.0.jar"       2>/dev/null || true
        curl -sSfL -o "$JJARS/jackson-annotations-2.22.jar"   "$B/jackson-annotations/2.22/jackson-annotations-2.22.jar"     2>/dev/null || true
    fi
    if command -v javac >/dev/null 2>&1 && [[ -f "$JJARS/jackson-databind-2.22.0.jar" ]]; then
        OUT="$BUILD/java-codec"; SRC="$DIR/java/codec/JsonDom.java"
        if [[ ! -f "$OUT/JsonDom.class" || "$SRC" -nt "$OUT/JsonDom.class" ]]; then
            mkdir -p "$OUT"; javac -cp "$JCP" -d "$OUT" "$SRC" >/tmp/profile-codec-java.log 2>&1 || true
        fi
        if [[ -f "$OUT/JsonDom.class" ]]; then
            PROFILE_LANG_VERSION="$(java -version 2>&1 | head -1 | strip)" PROFILE_JACKSON_VERSION=2.22.0 \
                java -cp "$OUT:$JCP" JsonDom || skip_lang java jackson-databind "java codec runner crashed"
        else
            skip_lang java jackson-databind "javac build failed (see /tmp/profile-codec-java.log)"
        fi
    elif ! command -v javac >/dev/null 2>&1; then
        skip_lang java jackson-databind "javac not installed"
    else
        skip_lang java jackson-databind "jackson jars missing (vendor/jackson; needs network to fetch)"
    fi
fi
