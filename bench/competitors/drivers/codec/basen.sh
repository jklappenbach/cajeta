#!/usr/bin/env bash
# codec/base64-* and codec/base32-* competitors (bench/codec/Base64.md,
# Base32.md): encode 1 MiB (data[i] = (i*31+7) & 0xFF) and decode it, 1e3
# trials unless PROFILE_TRIALS says otherwise, one program per language under
# competitors/<lang>/codec/basen/:
#   cpp     simdutf (Base64 only; the Base32 page names no C++ library)
#   rust    base64 (STANDARD) and data-encoding (BASE32)
#   go      encoding/base64, encoding/base32
#   java    java.util.Base64, Apache commons-codec Base32
#   python  base64 (b64encode/b64decode, b32encode/b32decode)
# A missing toolchain or library is a skipped row per id with the reason.
# Env: PROFILE_RUN_ID, PROFILE_RUN_TS, PROFILE_WARMUP (3), PROFILE_TRIALS
# (1000), PROFILE_LANGS (space-list filter).
set -uo pipefail

DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )/../.." &> /dev/null && pwd )"   # competitors/
BUILD="${DIR}/.build"; mkdir -p "$BUILD"
LOGS="${DIR}/logs"; mkdir -p "$LOGS"
LANGS="${PROFILE_LANGS:-rust cpp go python java}"
export PATH="$HOME/.cargo/bin:$PATH"
strip() { tr -d ',"'; }
want() { case " $LANGS " in *" $1 "*) return 0;; *) return 1;; esac; }
N=1048576

skip_ids() {   # lang lib reason ids...
    local lang="$1" lib="$2" reason="$3" rid="${PROFILE_RUN_ID:-local}" ts="${PROFILE_RUN_TS:-}" b; shift 3
    for b in "$@"; do
        echo "1,${rid},${ts},${b},codec,,${N},,${N},${lang},,${lib},,,0,0,-1,-1,-1,-1,,,-1,-1,-1,-1,-1,skipped,,${reason},"
    done
}
ALL="base64-encode base64-decode base32-encode base32-decode"

if want rust; then
    if command -v cargo >/dev/null 2>&1; then
        if ( cd "$DIR/rust/codec/basen" && RUSTFLAGS="-C target-cpu=native" cargo build --release >"$LOGS/codec-basen-rust.log" 2>&1 ); then
            PROFILE_LANG_VERSION="$(rustc --version | strip)" "$DIR/rust/codec/basen/target/release/codec-basen" \
                || skip_ids rust base64 "rust basen runner crashed" $ALL
        else skip_ids rust base64 "cargo build failed (logs/codec-basen-rust.log)" $ALL; fi
    else skip_ids rust base64 "cargo not installed" $ALL; fi
fi

if want cpp; then
    SU="$DIR/third_party/simdutf"
    if command -v g++ >/dev/null 2>&1 && [[ -f "$SU/singleheader/amalgamate.py" ]]; then
        BIN="$BUILD/cpp-codec-basen"; SRC="$DIR/cpp/codec/basen/basen.cpp"; AM="$BUILD/simdutf"
        if [[ ! -f "$AM/simdutf.cpp" ]]; then
            python3 "$SU/singleheader/amalgamate.py" --output-dir "$AM" --no-zip --no-readme >"$LOGS/codec-basen-cpp.log" 2>&1
        fi
        if [[ ! -x "$BIN" || "$SRC" -nt "$BIN" || "$DIR/cpp/codec/basen/row.h" -nt "$BIN" ]]; then
            g++ -std=c++17 -O3 -march=native -DNDEBUG -I "$AM" "$SRC" "$AM/simdutf.cpp" -o "$BIN" >>"$LOGS/codec-basen-cpp.log" 2>&1 || rm -f "$BIN"
        fi
        if [[ -x "$BIN" ]]; then
            PROFILE_LANG_VERSION="g++$(g++ -dumpfullversion | cut -d. -f1)" "$BIN" || skip_ids cpp simdutf "cpp basen runner crashed" base64-encode base64-decode
        else skip_ids cpp simdutf "g++ build failed (logs/codec-basen-cpp.log)" base64-encode base64-decode; fi
    else skip_ids cpp simdutf "g++ or the pinned simdutf checkout missing (run competitors/setup.sh)" base64-encode base64-decode; fi
fi

if want go; then
    if command -v go >/dev/null 2>&1; then
        if ( cd "$DIR/go/codec/basen" && go build -o "$BUILD/go-codec-basen" . >"$LOGS/codec-basen-go.log" 2>&1 ); then
            PROFILE_LANG_VERSION="$(go version | strip)" "$BUILD/go-codec-basen" || skip_ids go encoding/base64 "go basen runner failed" $ALL
        else skip_ids go encoding/base64 "go build failed (logs/codec-basen-go.log)" $ALL; fi
    else skip_ids go encoding/base64 "go not installed" $ALL; fi
fi

if want python; then
    PY="$DIR/python/.venv/bin/python"; [[ -x "$PY" ]] || PY="$(command -v python3 || true)"
    if [[ -n "$PY" ]]; then
        PROFILE_LANG_VERSION="$("$PY" --version 2>&1 | strip)" "$PY" "$DIR/python/codec/basen/basen_bench.py" \
            || skip_ids python base64 "python basen runner crashed" $ALL
    else skip_ids python base64 "python3 not installed" $ALL; fi
fi

if want java; then
    JBIN="$HOME/.sdkman/candidates/java/current/bin"; [[ -x "$JBIN/javac" ]] || JBIN="$(dirname "$(command -v javac 2>/dev/null || echo /nonexistent/javac)")"
    CC="$DIR/third_party/java/commons-codec/commons-codec/1.23.0/commons-codec-1.23.0.jar"
    if [[ -x "$JBIN/javac" && -f "$CC" ]]; then
        OUT="$BUILD/java-codec-basen"; mkdir -p "$OUT"
        if [[ ! -f "$OUT/CodecBasen.class" || -n "$(find "$DIR/java/codec/basen" -name '*.java' -newer "$OUT/CodecBasen.class")" ]]; then
            "$JBIN/javac" -cp "$CC" -d "$OUT" "$DIR"/java/codec/basen/*.java >"$LOGS/codec-basen-java.log" 2>&1 || rm -f "$OUT/CodecBasen.class"
        fi
        if [[ -f "$OUT/CodecBasen.class" ]]; then
            PROFILE_LANG_VERSION="$("$JBIN/java" -version 2>&1 | head -1 | strip)" "$JBIN/java" -cp "$OUT:$CC" CodecBasen \
                || skip_ids java java.util.Base64 "java basen runner crashed" $ALL
        else skip_ids java java.util.Base64 "javac build failed (logs/codec-basen-java.log)" $ALL; fi
    else skip_ids java java.util.Base64 "javac or the pinned commons-codec jar missing (run competitors/setup.sh)" $ALL; fi
fi
