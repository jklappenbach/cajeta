#!/usr/bin/env bash
# Time competitor area runner. Builds + runs each present language's time runner
# (time-instant-arith 1M / time-localdate-arith 100K, same sizes + math as the
# Cajeta side) and streams schema-conformant CSV rows — the same benchmark names,
# so they line up in the report's time table.
#
# Libraries: rust (chrono), cpp (C++20 std::chrono), go (time), python (datetime).
# Cross-check = the exact closed-form epoch sums. Missing toolchain/lib ⇒
# `skipped` row with a reason.
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
    local rid="${PROFILE_RUN_ID:-local}" ts="${PROFILE_RUN_TS:-}" b sz
    for b in time-instant-arith time-localdate-arith; do
        sz=1000000; [[ "$b" == "time-localdate-arith" ]] && sz=100000
        echo "1,${rid},${ts},${b},time,,${sz},,${sz},${lang},,${lib},,,0,0,-1,-1,-1,-1,,,-1,-1,-1,-1,-1,skipped,,${reason},"
    done
}

want() { case " $LANGS " in *" $1 "*) return 0;; *) return 1;; esac; }

# ---- rust (chrono) ----
if want rust; then
    if command -v cargo >/dev/null 2>&1; then
        if ( cd "$DIR/rust/time" && cargo build --release >/tmp/profile-time-rust.log 2>&1 ); then
            PROFILE_LANG_VERSION="$(rustc --version | strip)" "$DIR/rust/time/target/release/profile-time" \
                || skip_lang rust chrono "rust time runner crashed"
        else
            skip_lang rust chrono "cargo build failed (see /tmp/profile-time-rust.log)"
        fi
    else
        skip_lang rust chrono "cargo not installed"
    fi
fi

# ---- cpp (C++20 std::chrono) ----
if want cpp; then
    if command -v g++ >/dev/null 2>&1; then
        BIN="$BUILD/cpp-time"
        if [[ ! -x "$BIN" || "$DIR/cpp/time/time_bench.cpp" -nt "$BIN" ]]; then
            g++ -O3 -march=native -std=c++20 -DNDEBUG "$DIR/cpp/time/time_bench.cpp" \
                -o "$BIN" >/tmp/profile-time-cpp.log 2>&1 || true
        fi
        if [[ -x "$BIN" ]]; then
            PROFILE_LANG_VERSION="$(g++ --version | head -1 | strip)" "$BIN" \
                || skip_lang cpp std::chrono "cpp time runner crashed"
        else
            skip_lang cpp std::chrono "g++ build failed — needs C++20 std::chrono (see /tmp/profile-time-cpp.log)"
        fi
    else
        skip_lang cpp std::chrono "g++ not installed"
    fi
fi

# ---- go ----
if want go; then
    if command -v go >/dev/null 2>&1; then
        ( cd "$DIR/go/time" && PROFILE_LANG_VERSION="$(go version | strip)" go run . 2>/tmp/profile-time-go.log ) \
            || skip_lang go time "go build/run failed (see /tmp/profile-time-go.log)"
    else
        skip_lang go time "go not installed"
    fi
fi

# ---- python ----
if want python; then
    PY="$DIR/python/.venv/bin/python"
    [[ -x "$PY" ]] || PY="$(command -v python3 || true)"
    if [[ -n "$PY" ]]; then
        PROFILE_LANG_VERSION="$("$PY" --version 2>&1 | strip)" "$PY" "$DIR/python/time/time_bench.py" \
            || skip_lang python datetime "python time runner crashed"
    else
        skip_lang python datetime "python3 not installed"
    fi
fi

# ---- java: java.time (Instant/LocalDate immutable value objects, JDK stdlib) ----
if want java; then
    if command -v javac >/dev/null 2>&1; then
        OUT="$BUILD/java-time"; SRC="$DIR/java/time/TimeBench.java"
        if [[ ! -f "$OUT/TimeBench.class" || "$SRC" -nt "$OUT/TimeBench.class" ]]; then
            mkdir -p "$OUT"; javac -d "$OUT" "$SRC" >/tmp/profile-time-java.log 2>&1 || true
        fi
        if [[ -f "$OUT/TimeBench.class" ]]; then
            PROFILE_LANG_VERSION="$(java -version 2>&1 | head -1 | strip)" java -cp "$OUT" TimeBench \
                || skip_lang java java.time "java time runner crashed"
        else
            skip_lang java java.time "javac build failed (see /tmp/profile-time-java.log)"
        fi
    else
        skip_lang java java.time "javac not installed"
    fi
fi
