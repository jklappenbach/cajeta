#!/usr/bin/env bash
# String competitor area runner. Builds + runs each present language's string
# runner over the same ~360 KB ASCII text the Cajeta side uses and streams
# schema-conformant CSV rows (competitors/columns.txt order) to stdout — the same
# string-search/string-replace/string-uppercase/string-build-concat benchmark
# names, so they line up in the report's string table.
#
# Libraries: rust (stdlib str + memchr for search), cpp (std::string idioms),
# go (strings package), python (str methods). Missing toolchain ⇒ `skipped` row
# with a reason. Diagnostic contrast: Cajeta's build-concat is O(N²) (no
# StringBuilder); every competitor uses an O(N) builder.
#
# Env: PROFILE_RUN_ID, PROFILE_RUN_TS, PROFILE_WARMUP (3), PROFILE_TRIALS (10),
#      PROFILE_LANGS (space-list filter).
set -uo pipefail

DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
BUILD="${DIR}/.build"; mkdir -p "$BUILD"
LANGS="${PROFILE_LANGS:-rust cpp go python java}"
strip() { tr -d ',"'; }
TEXTLEN=360448
BUILDLEN=4000

skip_lang() {
    local lang="$1" lib="$2" reason="$3"
    local rid="${PROFILE_RUN_ID:-local}" ts="${PROFILE_RUN_TS:-}"
    local b len
    for b in string-search string-replace string-uppercase string-build-concat; do
        len=$TEXTLEN; [[ "$b" == "string-build-concat" ]] && len=$BUILDLEN
        echo "1,${rid},${ts},${b},string,,${len},,${len},${lang},,${lib},,,0,0,-1,-1,-1,-1,,,-1,-1,-1,-1,-1,skipped,,${reason},"
    done
}

want() { case " $LANGS " in *" $1 "*) return 0;; *) return 1;; esac; }

# ---- rust (stdlib + memchr) ----
if want rust; then
    if command -v cargo >/dev/null 2>&1; then
        if ( cd "$DIR/rust/string" && cargo build --release >/tmp/profile-string-rust.log 2>&1 ); then
            PROFILE_LANG_VERSION="$(rustc --version | strip)" "$DIR/rust/string/target/release/profile-string" \
                || skip_lang rust rust "rust string runner crashed"
        else
            skip_lang rust rust "cargo build failed (see /tmp/profile-string-rust.log)"
        fi
    else
        skip_lang rust rust "cargo not installed"
    fi
fi

# ---- cpp (std::string) ----
if want cpp; then
    if command -v g++ >/dev/null 2>&1; then
        BIN="$BUILD/cpp-string"
        if [[ ! -x "$BIN" || "$DIR/cpp/string/string_bench.cpp" -nt "$BIN" ]]; then
            g++ -O3 -march=native -DNDEBUG "$DIR/cpp/string/string_bench.cpp" \
                -o "$BIN" >/tmp/profile-string-cpp.log 2>&1 || true
        fi
        if [[ -x "$BIN" ]]; then
            PROFILE_LANG_VERSION="$(g++ --version | head -1 | strip)" "$BIN" \
                || skip_lang cpp cpp "cpp string runner crashed"
        else
            skip_lang cpp cpp "g++ build failed (see /tmp/profile-string-cpp.log)"
        fi
    else
        skip_lang cpp cpp "g++ not installed"
    fi
fi

# ---- go ----
if want go; then
    if command -v go >/dev/null 2>&1; then
        ( cd "$DIR/go/string" && PROFILE_LANG_VERSION="$(go version | strip)" go run . 2>/tmp/profile-string-go.log ) \
            || skip_lang go go "go build/run failed (see /tmp/profile-string-go.log)"
    else
        skip_lang go go "go not installed"
    fi
fi

# ---- python ----
if want python; then
    PY="$DIR/python/.venv/bin/python"
    [[ -x "$PY" ]] || PY="$(command -v python3 || true)"
    if [[ -n "$PY" ]]; then
        PROFILE_LANG_VERSION="$("$PY" --version 2>&1 | strip)" "$PY" "$DIR/python/string/string_bench.py" \
            || skip_lang python python "python string runner crashed"
    else
        skip_lang python python "python3 not installed"
    fi
fi

# ---- java: String idioms (indexOf / replace / toUpperCase / StringBuilder, JDK stdlib) ----
if want java; then
    if command -v javac >/dev/null 2>&1; then
        OUT="$BUILD/java-string"; SRC="$DIR/java/string/StringBench.java"
        if [[ ! -f "$OUT/StringBench.class" || "$SRC" -nt "$OUT/StringBench.class" ]]; then
            mkdir -p "$OUT"; javac -d "$OUT" "$SRC" >/tmp/profile-string-java.log 2>&1 || true
        fi
        if [[ -f "$OUT/StringBench.class" ]]; then
            PROFILE_LANG_VERSION="$(java -version 2>&1 | head -1 | strip)" java -cp "$OUT" StringBench \
                || skip_lang java java "java string runner crashed"
        else
            skip_lang java java "javac build failed (see /tmp/profile-string-java.log)"
        fi
    else
        skip_lang java java "javac not installed"
    fi
fi
