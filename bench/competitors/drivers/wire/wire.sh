#!/usr/bin/env bash
# wire/ competitors (benchmark-reference unit 13, spec §6.10; bench/wire/*.md):
# one program per language under competitors/<lang>/wire/wire/, each running the
# wire/ ids the pages name for that language, on the 100-field record of
# bench/scripts/gen_wire.py (schemas + expected constants in
# competitors/cpp/wire/schema/). Rows are v1 (31 columns); run.sh lifts them.
#
#   cpp   protobuf v36.2 (generated + DynamicMessage), FlatBuffers v25.12.19,
#         Cap'n Proto v1.5.0 (generated + DynamicStruct), zlib, libzstd
#         encoder-encode encoder-decode struct-view-read schemaencoder-encode
#         schemaencoder-decode compressstream-roundtrip [zlib, zstd]
#   java  protobuf-java 4.36.2, Avro 1.12.2 GenericDatum, java.util.zip, zstd-jni
#         encoder-encode encoder-decode schemaencoder-encode schemaencoder-decode
#         compressstream-roundtrip [zlib, zstd]
#   rust  bincode 1.3, flate2, zstd     encoder-encode encoder-decode compressstream-roundtrip [zlib, zstd]
#   go    compress/flate, klauspost/compress/zstd     compressstream-roundtrip [zlib, zstd]
#
# protoc, flatc and capnp come from the libraries setup.sh builds into
# third_party/<name>-install; their generated code goes to competitors/.build.
# A missing toolchain, library or failed build is a `skipped` row per id.
# Env: PROFILE_RUN_ID, PROFILE_RUN_TS, PROFILE_WARMUP, PROFILE_TRIALS, PROFILE_LANGS,
#      CAJETA_BENCH_SMOKE (smoke sizes, said in notes).
set -uo pipefail

DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )/../.." &> /dev/null && pwd )"   # competitors/
BUILD="${DIR}/.build"; mkdir -p "$BUILD"
LOGD="${TMPDIR:-/tmp}"
TP="$DIR/third_party"
SCHEMA="$DIR/cpp/wire/schema"
export WIRE_EXPECT="$SCHEMA/expect.tsv" WIRE_AVSC="$SCHEMA/record.avsc"
LANGS="${PROFILE_LANGS:-cpp java rust go}"
strip() { tr -d ',"'; }
[[ -d "$HOME/.cargo/bin" ]] && export PATH="$HOME/.cargo/bin:$PATH"
want() { case " $LANGS " in *" $1 "*) return 0;; *) return 1;; esac; }

REC_IDS="encoder-encode encoder-decode"
SCH_IDS="schemaencoder-encode schemaencoder-decode"
CPP_IDS="$REC_IDS struct-view-read $SCH_IDS compressstream-roundtrip:zlib compressstream-roundtrip:zstd"
JAVA_IDS="$REC_IDS $SCH_IDS compressstream-roundtrip:zlib compressstream-roundtrip:zstd"
RUST_IDS="$REC_IDS compressstream-roundtrip:zlib compressstream-roundtrip:zstd"
GO_IDS="compressstream-roundtrip:zlib compressstream-roundtrip:zstd"

skip_lang() {   # skip_lang <lang> <lib> <reason> <id[:variant]...>
    local lang="$1" lib="$2" reason="$3" rid="${PROFILE_RUN_ID:-local}" ts="${PROFILE_RUN_TS:-}" b v
    shift 3
    for b in "$@"; do
        v=""; [[ "$b" == *:* ]] && { v="${b#*:}"; b="${b%%:*}"; }
        echo "1,${rid},${ts},${b},wire,,0,,0,${lang},,${lib},,,0,0,-1,-1,-1,-1,,,-1,-1,-1,-1,-1,skipped,,${reason},${v}"
    done
}

# ---- cpp: protobuf, FlatBuffers, Cap'n Proto, zlib, zstd ----
if want cpp; then
    PB="$TP/protobuf-install" FB="$TP/flatbuffers-install" CP="$TP/capnproto-install"
    SRC="$DIR/cpp/wire/wire/wire_bench.cpp"; GEN="$BUILD/cpp-wire-gen"; BIN="$BUILD/cpp-wire"
    if ! command -v g++ >/dev/null 2>&1; then
        skip_lang cpp protobuf "g++ not installed" $CPP_IDS
    elif [[ ! -x "$PB/bin/protoc" || ! -x "$FB/bin/flatc" || ! -x "$CP/bin/capnp" ]]; then
        skip_lang cpp protobuf "protobuf/flatbuffers/capnproto -install missing under competitors/third_party (run competitors/setup.sh)" $CPP_IDS
    elif [[ ! -f /usr/include/zlib.h || ! -f /usr/include/zstd.h ]]; then
        skip_lang cpp zlib "zlib.h / zstd.h missing (apt install zlib1g-dev libzstd-dev)" $CPP_IDS
    else
        if [[ ! -x "$BIN" || "$SRC" -nt "$BIN" || "$DIR/cpp/wire/wire/wire_fields.h" -nt "$BIN" || "$DIR/cpp/wire/wire/row.h" -nt "$BIN" || "$SCHEMA/record.proto" -nt "$BIN" || "$SCHEMA/record.fbs" -nt "$BIN" ]]; then
            rm -rf "$GEN"; mkdir -p "$GEN"
            {
                "$PB/bin/protoc" -I"$SCHEMA" --cpp_out="$GEN" "$SCHEMA/record.proto" &&
                cp "$SCHEMA/record.fbs" "$GEN/record.fbs" &&
                "$FB/bin/flatc" --cpp --gen-object-api -o "$GEN" "$GEN/record.fbs" &&
                PATH="$CP/bin:$PATH" "$CP/bin/capnp" compile -I"$CP/include" --src-prefix="$SCHEMA" -oc++:"$GEN" "$SCHEMA/record.capnp" &&
                g++ -std=c++17 -O3 -march=native -DNDEBUG -I"$GEN" -I"$DIR/cpp/wire/wire" -I"$PB/include" -I"$FB/include" -I"$CP/include" \
                    "$SRC" "$GEN/record.pb.cc" "$GEN/record.capnp.c++" -o "$BIN" \
                    -L"$CP/lib" -lcapnp -lkj \
                    $(PKG_CONFIG_PATH="$PB/lib/pkgconfig" pkg-config --static --libs protobuf) \
                    -lz -lzstd -lpthread
            } >"$LOGD/cajeta-wire-cpp-build.log" 2>&1 || rm -f "$BIN"
        fi
        if [[ -x "$BIN" ]]; then
            PROFILE_LANG_VERSION="$(g++ --version | head -1 | strip)" "$BIN" 2>"$LOGD/cajeta-wire-cpp.log" \
                || skip_lang cpp protobuf "cpp wire runner crashed (see $LOGD/cajeta-wire-cpp.log)" $CPP_IDS
        else
            skip_lang cpp protobuf "g++ build failed (see $LOGD/cajeta-wire-cpp-build.log)" $CPP_IDS
        fi
    fi
fi

# ---- java: protobuf-java, Avro, java.util.zip, zstd-jni (jars pinned by setup.sh) ----
if want java; then
    J="$TP/java"
    JCP="$J/com/google/protobuf/protobuf-java/4.36.2/protobuf-java-4.36.2.jar:$J/org/apache/avro/avro/1.12.2/avro-1.12.2.jar"
    JCP="$JCP:$J/com/fasterxml/jackson/core/jackson-core/2.22.0/jackson-core-2.22.0.jar:$J/com/fasterxml/jackson/core/jackson-databind/2.22.0/jackson-databind-2.22.0.jar"
    JCP="$JCP:$J/com/fasterxml/jackson/core/jackson-annotations/2.22/jackson-annotations-2.22.jar:$J/org/slf4j/slf4j-api/2.0.18/slf4j-api-2.0.18.jar"
    JCP="$JCP:$J/com/github/luben/zstd-jni/1.5.7-24/zstd-jni-1.5.7-24.jar"
    missing=""
    for f in ${JCP//:/ }; do [[ -f "$f" ]] || missing="$missing $(basename "$f")"; done
    PROTOC="$TP/protobuf-install/bin/protoc"
    if ! command -v javac >/dev/null 2>&1; then
        skip_lang java protobuf-java "javac not installed" $JAVA_IDS
    elif [[ -n "$missing" ]]; then
        skip_lang java protobuf-java "pinned jars missing:$missing (run competitors/setup.sh)" $JAVA_IDS
    elif [[ ! -x "$PROTOC" ]]; then
        skip_lang java protobuf-java "protoc missing (third_party/protobuf-install; run competitors/setup.sh)" $JAVA_IDS
    else
        OUT="$BUILD/java-wire"; JS="$DIR/java/wire/wire"
        if [[ ! -f "$OUT/WireBench.class" || -n "$(find "$JS" "$SCHEMA/record.proto" -newer "$OUT/WireBench.class" -print -quit)" ]]; then
            rm -rf "$OUT"; mkdir -p "$OUT/gen"
            { "$PROTOC" -I"$SCHEMA" --java_out="$OUT/gen" "$SCHEMA/record.proto" &&
              javac -cp "$JCP" -d "$OUT" $(find "$OUT/gen" -name '*.java') "$JS"/*.java; } \
                >"$LOGD/cajeta-wire-java-build.log" 2>&1 || rm -f "$OUT/WireBench.class"
        fi
        if [[ -f "$OUT/WireBench.class" ]]; then
            PROFILE_LANG_VERSION="$(java -version 2>&1 | head -1 | strip)" java -Xmx8g --enable-native-access=ALL-UNNAMED -cp "$OUT:$JCP" WireBench 2>"$LOGD/cajeta-wire-java.log" \
                || skip_lang java protobuf-java "java wire runner crashed (see $LOGD/cajeta-wire-java.log)" $JAVA_IDS
        else
            skip_lang java protobuf-java "javac build failed (see $LOGD/cajeta-wire-java-build.log)" $JAVA_IDS
        fi
    fi
fi

# ---- rust: bincode, flate2, zstd (Cargo.lock pins them) ----
if want rust; then
    if command -v cargo >/dev/null 2>&1; then
        RD="$DIR/rust/wire/wire"
        if ( cd "$RD" && RUSTFLAGS="-C target-cpu=native" cargo build --release --locked >"$LOGD/cajeta-wire-rust-build.log" 2>&1 ); then
            PROFILE_LANG_VERSION="$(rustc --version | strip)" "$RD/target/release/wire-wire" 2>"$LOGD/cajeta-wire-rust.log" \
                || skip_lang rust bincode "rust wire runner returned Err (see $LOGD/cajeta-wire-rust.log)" $RUST_IDS
        else
            skip_lang rust bincode "cargo build failed (see $LOGD/cajeta-wire-rust-build.log)" $RUST_IDS
        fi
    else
        skip_lang rust bincode "cargo not installed" $RUST_IDS
    fi
fi

# ---- go: compress/flate, klauspost/compress/zstd (go.mod pins the version; go.sum is gitignored, so the build is -mod=mod and verifies against the checksum database) ----
if want go; then
    if command -v go >/dev/null 2>&1; then
        GD="$DIR/go/wire/wire"; GBIN="$BUILD/go-wire"
        if ( cd "$GD" && GOAMD64="${GOAMD64:-v3}" go build -mod=mod -o "$GBIN" . >"$LOGD/cajeta-wire-go-build.log" 2>&1 ); then
            PROFILE_LANG_VERSION="$(go version | strip)" "$GBIN" 2>"$LOGD/cajeta-wire-go.log" \
                || skip_lang go compress/flate "go wire runner returned an error (see $LOGD/cajeta-wire-go.log)" $GO_IDS
        else
            skip_lang go compress/flate "go build failed (see $LOGD/cajeta-wire-go-build.log)" $GO_IDS
        fi
    else
        skip_lang go compress/flate "go not installed" $GO_IDS
    fi
fi
exit 0
