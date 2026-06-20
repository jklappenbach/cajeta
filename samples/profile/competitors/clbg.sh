#!/usr/bin/env bash
# CLBG competitor area runner. Builds + runs each present language's runner for
# the three Computer Language Benchmarks Game classics (mandelbrot 800² /
# fannkuch-redux N=10 / spectral-norm N=100) at the same sizes the Cajeta side
# uses, verified against the canonical checksums (254099 / 38+73196 / 1.274224),
# and streams schema-conformant CSV rows — the same benchmark names, so they line
# up in the report's clbg table. Pure scalar stdlib (rust/cpp/go); python uses
# numpy for mandelbrot, pure CPython for fannkuch/spectral-norm.
#
# Env: PROFILE_RUN_ID, PROFILE_RUN_TS, PROFILE_WARMUP (3), PROFILE_TRIALS (10),
#      PROFILE_LANGS (space-list filter).
set -uo pipefail

DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
BUILD="${DIR}/.build"; mkdir -p "$BUILD"
LANGS="${PROFILE_LANGS:-rust cpp go python}"
strip() { tr -d ',"'; }

skip_lang() {
    local lang="$1" lib="$2" reason="$3"
    local rid="${PROFILE_RUN_ID:-local}" ts="${PROFILE_RUN_TS:-}" b sz
    for b in clbg-mandelbrot clbg-fannkuch-redux clbg-spectral-norm; do
        case "$b" in clbg-mandelbrot) sz=640000;; clbg-fannkuch-redux) sz=10;; *) sz=100;; esac
        echo "1,${rid},${ts},${b},clbg,,${sz},,${sz},${lang},,${lib},,,0,0,-1,-1,-1,-1,,,-1,-1,-1,-1,-1,skipped,,${reason},"
    done
}

want() { case " $LANGS " in *" $1 "*) return 0;; *) return 1;; esac; }

# ---- rust ----
if want rust; then
    if command -v cargo >/dev/null 2>&1; then
        if ( cd "$DIR/rust/clbg" && cargo build --release >/tmp/profile-clbg-rust.log 2>&1 ); then
            PROFILE_LANG_VERSION="$(rustc --version | strip)" "$DIR/rust/clbg/target/release/profile-clbg" \
                || skip_lang rust scalar "rust clbg runner crashed"
        else
            skip_lang rust scalar "cargo build failed (see /tmp/profile-clbg-rust.log)"
        fi
    else
        skip_lang rust scalar "cargo not installed"
    fi
fi

# ---- cpp ----
if want cpp; then
    if command -v g++ >/dev/null 2>&1; then
        BIN="$BUILD/cpp-clbg"
        if [[ ! -x "$BIN" || "$DIR/cpp/clbg/clbg_bench.cpp" -nt "$BIN" ]]; then
            g++ -O3 -march=native -DNDEBUG "$DIR/cpp/clbg/clbg_bench.cpp" \
                -o "$BIN" >/tmp/profile-clbg-cpp.log 2>&1 || true
        fi
        if [[ -x "$BIN" ]]; then
            PROFILE_LANG_VERSION="$(g++ --version | head -1 | strip)" "$BIN" \
                || skip_lang cpp scalar "cpp clbg runner crashed"
        else
            skip_lang cpp scalar "g++ build failed (see /tmp/profile-clbg-cpp.log)"
        fi
    else
        skip_lang cpp scalar "g++ not installed"
    fi
fi

# ---- go ----
if want go; then
    if command -v go >/dev/null 2>&1; then
        ( cd "$DIR/go/clbg" && PROFILE_LANG_VERSION="$(go version | strip)" go run . 2>/tmp/profile-clbg-go.log ) \
            || skip_lang go scalar "go build/run failed (see /tmp/profile-clbg-go.log)"
    else
        skip_lang go scalar "go not installed"
    fi
fi

# ---- python (numpy mandelbrot + pure fannkuch/spectral) ----
if want python; then
    PY="$DIR/python/.venv/bin/python"
    [[ -x "$PY" ]] || PY="$(command -v python3 || true)"
    if [[ -n "$PY" ]]; then
        PROFILE_LANG_VERSION="$("$PY" --version 2>&1 | strip)" "$PY" "$DIR/python/clbg/clbg_bench.py" \
            || skip_lang python cpython "python clbg runner crashed"
    else
        skip_lang python cpython "python3 not installed"
    fi
fi
