#!/usr/bin/env bash
# Stream competitor area runner. Builds + runs each present language's stream
# runner (stream-filter-map-reduce sequential + stream-parallel-reduce, over
# xs[i]=i%1000, N=1M, same as the Cajeta side) and streams schema-conformant CSV
# rows — the same benchmark names, so they line up in the report's stream table.
#
# Libraries: rust (Iterator + rayon), cpp (hand loop + OpenMP), go (loop +
# goroutine fan-out), python (genexpr + GIL-bound sum). Cross-check = the exact
# reference sums. Missing toolchain ⇒ `skipped` row with a reason.
#
# Env: PROFILE_RUN_ID, PROFILE_RUN_TS, PROFILE_WARMUP (3), PROFILE_TRIALS (10),
#      PROFILE_LANGS (space-list filter).
set -uo pipefail

DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
BUILD="${DIR}/.build"; mkdir -p "$BUILD"
LANGS="${PROFILE_LANGS:-rust cpp go python java}"
strip() { tr -d ',"'; }

skip_lang() {
    local lang="$1" lib="$2" reason="$3"
    local rid="${PROFILE_RUN_ID:-local}" ts="${PROFILE_RUN_TS:-}" b
    for b in stream-filter-map-reduce stream-parallel-reduce; do
        echo "1,${rid},${ts},${b},stream,,1000000,,1000000,${lang},,${lib},,,0,0,-1,-1,-1,-1,,,-1,-1,-1,-1,-1,skipped,,${reason},"
    done
}

want() { case " $LANGS " in *" $1 "*) return 0;; *) return 1;; esac; }

# ---- rust (Iterator + rayon) ----
if want rust; then
    if command -v cargo >/dev/null 2>&1; then
        if ( cd "$DIR/rust/stream" && cargo build --release >/tmp/profile-stream-rust.log 2>&1 ); then
            PROFILE_LANG_VERSION="$(rustc --version | strip)" "$DIR/rust/stream/target/release/profile-stream" \
                || skip_lang rust rust "rust stream runner crashed"
        else
            skip_lang rust rust "cargo build failed (see /tmp/profile-stream-rust.log)"
        fi
    else
        skip_lang rust rust "cargo not installed"
    fi
fi

# ---- cpp (hand loop + OpenMP) ----
if want cpp; then
    if command -v g++ >/dev/null 2>&1; then
        BIN="$BUILD/cpp-stream"
        if [[ ! -x "$BIN" || "$DIR/cpp/stream/stream_bench.cpp" -nt "$BIN" ]]; then
            g++ -O3 -march=native -fopenmp -DNDEBUG "$DIR/cpp/stream/stream_bench.cpp" \
                -o "$BIN" >/tmp/profile-stream-cpp.log 2>&1 || true
        fi
        if [[ -x "$BIN" ]]; then
            PROFILE_LANG_VERSION="$(g++ --version | head -1 | strip)" "$BIN" \
                || skip_lang cpp cpp "cpp stream runner crashed"
        else
            skip_lang cpp cpp "g++ build failed — needs OpenMP (see /tmp/profile-stream-cpp.log)"
        fi
    else
        skip_lang cpp cpp "g++ not installed"
    fi
fi

# ---- go ----
if want go; then
    if command -v go >/dev/null 2>&1; then
        ( cd "$DIR/go/stream" && GOAMD64=v4 PROFILE_LANG_VERSION="$(go version | strip)" go run . 2>/tmp/profile-stream-go.log ) \
            || skip_lang go go "go build/run failed (see /tmp/profile-stream-go.log)"
    else
        skip_lang go go "go not installed"
    fi
fi

# ---- python ----
if want python; then
    PY="$DIR/python/.venv/bin/python"
    [[ -x "$PY" ]] || PY="$(command -v python3 || true)"
    if [[ -n "$PY" ]]; then
        PROFILE_LANG_VERSION="$("$PY" --version 2>&1 | strip)" "$PY" "$DIR/python/stream/stream_bench.py" \
            || skip_lang python python "python stream runner crashed"
    else
        skip_lang python python "python3 not installed"
    fi
fi

# ---- java: java.util.stream (sequential LongStream filter→map→sum + .parallel() reduce) ----
if want java; then
    if command -v javac >/dev/null 2>&1; then
        OUT="$BUILD/java-stream"; SRC="$DIR/java/stream/StreamBench.java"
        if [[ ! -f "$OUT/StreamBench.class" || "$SRC" -nt "$OUT/StreamBench.class" ]]; then
            mkdir -p "$OUT"; javac -d "$OUT" "$SRC" >/tmp/profile-stream-java.log 2>&1 || true
        fi
        if [[ -f "$OUT/StreamBench.class" ]]; then
            PROFILE_LANG_VERSION="$(java -version 2>&1 | head -1 | strip)" java -cp "$OUT" StreamBench \
                || skip_lang java java "java stream runner crashed"
        else
            skip_lang java java "javac build failed (see /tmp/profile-stream-java.log)"
        fi
    else
        skip_lang java java "javac not installed"
    fi
fi
