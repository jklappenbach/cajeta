#!/usr/bin/env bash
# Math competitor area runner. Builds + runs each present language's math runner
# (saxpy / dot-product / matmul, f64, same sizes + init as the Cajeta side) and
# streams schema-conformant CSV rows (competitors/columns.txt order) to stdout —
# the same saxpy/dot-product/matmul benchmark names, so they line up in the
# report's math table.
#
# rust/cpp/go run the SAME scalar kernels Cajeta uses (so the row isolates
# auto-vectorization on identical work); python uses numpy (BLAS/SIMD) — the
# headline that shows what a tuned library buys. Missing toolchain/lib ⇒
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
    for b in saxpy dot-product matmul; do
        sz=1000000; [[ "$b" == "matmul" ]] && sz=40000
        echo "1,${rid},${ts},${b},math,,${sz},,${sz},${lang},,${lib},,,0,0,-1,-1,-1,-1,,,-1,-1,-1,-1,-1,skipped,,${reason},"
    done
}

want() { case " $LANGS " in *" $1 "*) return 0;; *) return 1;; esac; }

# ---- rust (scalar) ----
if want rust; then
    if command -v cargo >/dev/null 2>&1; then
        if ( cd "$DIR/rust/math" && cargo build --release >/tmp/profile-math-rust.log 2>&1 ); then
            PROFILE_LANG_VERSION="$(rustc --version | strip)" "$DIR/rust/math/target/release/profile-math" \
                || skip_lang rust scalar "rust math runner crashed"
        else
            skip_lang rust scalar "cargo build failed (see /tmp/profile-math-rust.log)"
        fi
    else
        skip_lang rust scalar "cargo not installed"
    fi
fi

# ---- cpp (scalar) ----
if want cpp; then
    if command -v g++ >/dev/null 2>&1; then
        BIN="$BUILD/cpp-math"
        if [[ ! -x "$BIN" || "$DIR/cpp/math/math_bench.cpp" -nt "$BIN" ]]; then
            g++ -O3 -march=native -DNDEBUG "$DIR/cpp/math/math_bench.cpp" \
                -o "$BIN" >/tmp/profile-math-cpp.log 2>&1 || true
        fi
        if [[ -x "$BIN" ]]; then
            PROFILE_LANG_VERSION="$(g++ --version | head -1 | strip)" "$BIN" \
                || skip_lang cpp scalar "cpp math runner crashed"
        else
            skip_lang cpp scalar "g++ build failed (see /tmp/profile-math-cpp.log)"
        fi
    else
        skip_lang cpp scalar "g++ not installed"
    fi
fi

# ---- go (scalar) ----
if want go; then
    if command -v go >/dev/null 2>&1; then
        ( cd "$DIR/go/math" && GOAMD64=v4 PROFILE_LANG_VERSION="$(go version | strip)" go run . 2>/tmp/profile-math-go.log ) \
            || skip_lang go scalar "go build/run failed (see /tmp/profile-math-go.log)"
    else
        skip_lang go scalar "go not installed"
    fi
fi

# ---- python (numpy) ----
if want python; then
    PY="$DIR/python/.venv/bin/python"
    [[ -x "$PY" ]] || PY="$(command -v python3 || true)"
    if [[ -n "$PY" ]]; then
        PROFILE_LANG_VERSION="$("$PY" --version 2>&1 | strip)" "$PY" "$DIR/python/math/math_bench.py" \
            || skip_lang python numpy "python math runner crashed"
    else
        skip_lang python numpy "python3 not installed"
    fi
fi

# ---- java (scalar; JIT auto-vectorized double[] loops, JDK stdlib) ----
if want java; then
    if command -v javac >/dev/null 2>&1; then
        OUT="$BUILD/java-math"; SRC="$DIR/java/math/MathBench.java"
        if [[ ! -f "$OUT/MathBench.class" || "$SRC" -nt "$OUT/MathBench.class" ]]; then
            mkdir -p "$OUT"; javac -d "$OUT" "$SRC" >/tmp/profile-math-java.log 2>&1 || true
        fi
        if [[ -f "$OUT/MathBench.class" ]]; then
            PROFILE_LANG_VERSION="$(java -version 2>&1 | head -1 | strip)" java -cp "$OUT" MathBench \
                || skip_lang java scalar "java math runner crashed"
        else
            skip_lang java scalar "javac build failed (see /tmp/profile-math-java.log)"
        fi
    else
        skip_lang java scalar "javac not installed"
    fi
fi
