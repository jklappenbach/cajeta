#!/usr/bin/env bash
# math/ numeric competitors (benchmark-reference unit 11, spec §6.8): one
# program per language under competitors/<lang>/math/numeric/, each running
# every math/ workload its page names for that language with the library the
# page names, at the sizes and with the checks of the Cajeta side. Rows are v1
# (31 columns); run.sh turns them into schema v2 with catalog ids math/<slug>.
#
#   python  NumPy + SciPy (venv)          tensor, linalg (scipy.linalg), fft (numpy.fft = pocketfft),
#                                         random, npy, poly, stats (scipy.stats), transform
#   cpp     Eigen, FFTW (float, built by setup.sh), pocketfft, <random>, plain C++
#   rust    ndarray, nalgebra, rustfft, rand/rand_distr, npyz, statrs, glam
#   go      gonum (mat, fourier, stat), math/rand/v2, plain Go
#   java    EJML, JTransforms, java.util.random, npy (org.jetbrains.bio), Commons Math, JOML
#
# A missing toolchain or library is a `skipped` row per id with the reason.
# CAJETA_BENCH_SMOKE=1 selects the smoke sizes (every program says which in notes).
# Env: PROFILE_RUN_ID, PROFILE_RUN_TS, PROFILE_WARMUP, PROFILE_TRIALS, PROFILE_LANGS,
#      MATH_IDS (space list of slugs to run), CAJETA_BENCH_SMOKE.
set -uo pipefail

DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )/../.." &> /dev/null && pwd )"   # competitors/
BUILD="${DIR}/.build"; mkdir -p "$BUILD"
LOGD="${TMPDIR:-/tmp}"
TP="$DIR/third_party"
LANGS="${PROFILE_LANGS:-python cpp rust go java}"
strip() { tr -d ',"'; }
# rustup installs user-level (setup.sh); a non-login shell may not have it on PATH.
[[ -d "$HOME/.cargo/bin" ]] && export PATH="$HOME/.cargo/bin:$PATH"
want() { case " $LANGS " in *" $1 "*) return 0;; *) return 1;; esac; }

T_IDS="tensor-elementwise tensor-broadcast tensor-reduce tensor-transpose tensor-cast"
L_IDS="linalg-solve linalg-inverse linalg-qr linalg-svd"
G_IDS="ray-sphere ray-aabb rotation-compose transform-apply"
PY_IDS="tensor-element-get tensor-element-set $T_IDS $L_IDS fft-1d fft-2d random-uniform random-normal npy-load-i32 npy-save-i32 poly-eval poly-fit stats-moments stats-histogram transform-apply"
CPP_IDS="tensor-element-get tensor-element-set $T_IDS $L_IDS fft-1d fft-2d random-uniform random-normal poly-eval poly-fit $G_IDS"
RUST_IDS="tensor-element-get tensor-element-set $T_IDS $L_IDS fft-1d fft-2d random-uniform random-normal npy-load-i32 npy-save-i32 poly-eval poly-fit stats-moments stats-histogram $G_IDS"
GO_IDS="$T_IDS $L_IDS fft-1d fft-2d random-uniform random-normal poly-eval poly-fit stats-moments stats-histogram $G_IDS"
JAVA_IDS="$T_IDS $L_IDS fft-1d fft-2d random-uniform random-normal npy-load-i32 npy-save-i32 poly-eval stats-moments stats-histogram $G_IDS"

skip_lang() {   # skip_lang <lang> <lib> <reason> <ids...>
    local lang="$1" lib="$2" reason="$3" rid="${PROFILE_RUN_ID:-local}" ts="${PROFILE_RUN_TS:-}" b
    shift 3
    for b in "$@"; do
        if [[ -n "${MATH_IDS:-}" && " $MATH_IDS " != *" $b "* ]]; then continue; fi
        echo "1,${rid},${ts},${b},math,,0,,0,${lang},,${lib},,,0,0,-1,-1,-1,-1,,,-1,-1,-1,-1,-1,skipped,,${reason},"
    done
}

# ---- python: NumPy + SciPy ----
if want python; then
    PY="$DIR/python/.venv/bin/python"
    if [[ -x "$PY" ]] && "$PY" -c 'import numpy, scipy' 2>/dev/null; then
        PROFILE_LANG_VERSION="$("$PY" --version 2>&1 | strip)" "$PY" "$DIR/python/math/numeric/numeric_bench.py" \
            2>"$LOGD/cajeta-math-numeric-python.log" \
            || skip_lang python numpy "python numeric runner crashed (see $LOGD/cajeta-math-numeric-python.log)" $PY_IDS
    else
        skip_lang python numpy "venv with numpy+scipy missing (run competitors/setup.sh)" $PY_IDS
    fi
fi

# ---- cpp: Eigen, FFTW, pocketfft, <random> ----
if want cpp; then
    SRC="$DIR/cpp/math/numeric/numeric_bench.cpp"; BIN="$BUILD/cpp-math-numeric"
    if ! command -v g++ >/dev/null 2>&1; then
        skip_lang cpp eigen "g++ not installed" $CPP_IDS
    elif [[ ! -f "$TP/eigen/Eigen/Dense" || ! -f "$TP/pocketfft/pocketfft_hdronly.h" || ! -f "$TP/fftw-install/lib/libfftw3f.a" ]]; then
        skip_lang cpp eigen "Eigen/pocketfft/FFTW (fftw-install) missing under competitors/third_party (run competitors/setup.sh)" $CPP_IDS
    else
        if [[ ! -x "$BIN" || "$SRC" -nt "$BIN" ]]; then
            g++ -std=c++17 -O3 -march=native -DNDEBUG -I"$TP/eigen" -I"$TP/pocketfft" -I"$TP/fftw-install/include" \
                "$SRC" -o "$BIN" "$TP/fftw-install/lib/libfftw3f.a" -lpthread >"$LOGD/cajeta-math-numeric-cpp-build.log" 2>&1 || rm -f "$BIN"
        fi
        if [[ -x "$BIN" ]]; then
            PROFILE_LANG_VERSION="$(g++ --version | head -1 | strip)" "$BIN" 2>"$LOGD/cajeta-math-numeric-cpp.log" \
                || skip_lang cpp eigen "cpp numeric runner crashed (see $LOGD/cajeta-math-numeric-cpp.log)" $CPP_IDS
        else
            skip_lang cpp eigen "g++ build failed (see $LOGD/cajeta-math-numeric-cpp-build.log)" $CPP_IDS
        fi
    fi
fi

# ---- rust: ndarray, nalgebra, rustfft, rand, npyz, statrs, glam (Cargo.lock pins them) ----
if want rust; then
    if command -v cargo >/dev/null 2>&1; then
        RD="$DIR/rust/math/numeric"
        if ( cd "$RD" && RUSTFLAGS="-C target-cpu=native" cargo build --release --locked >"$LOGD/cajeta-math-numeric-rust-build.log" 2>&1 ); then
            PROFILE_LANG_VERSION="$(rustc --version | strip)" "$RD/target/release/math-numeric" 2>"$LOGD/cajeta-math-numeric-rust.log" \
                || skip_lang rust ndarray "rust numeric runner crashed (see $LOGD/cajeta-math-numeric-rust.log)" $RUST_IDS
        else
            skip_lang rust ndarray "cargo build failed (see $LOGD/cajeta-math-numeric-rust-build.log)" $RUST_IDS
        fi
    else
        skip_lang rust ndarray "cargo not installed" $RUST_IDS
    fi
fi

# ---- go: gonum, math/rand/v2 (go.sum pins gonum) ----
if want go; then
    if command -v go >/dev/null 2>&1; then
        GD="$DIR/go/math/numeric"; GBIN="$BUILD/go-math-numeric"
        if ( cd "$GD" && GOAMD64="${GOAMD64:-v3}" go build -o "$GBIN" . >"$LOGD/cajeta-math-numeric-go-build.log" 2>&1 ); then
            PROFILE_LANG_VERSION="$(go version | strip)" "$GBIN" 2>"$LOGD/cajeta-math-numeric-go.log" \
                || skip_lang go gonum "go numeric runner crashed (see $LOGD/cajeta-math-numeric-go.log)" $GO_IDS
        else
            skip_lang go gonum "go build failed (see $LOGD/cajeta-math-numeric-go-build.log)" $GO_IDS
        fi
    else
        skip_lang go gonum "go not installed" $GO_IDS
    fi
fi

# ---- java: EJML, JTransforms, npy, Commons Math, JOML (jars pinned by setup.sh) ----
if want java; then
    J="$TP/java"
    JCP="$J/org/ejml/ejml-core/0.46.1/ejml-core-0.46.1.jar:$J/org/ejml/ejml-ddense/0.46.1/ejml-ddense-0.46.1.jar:$J/org/ejml/ejml-fdense/0.46.1/ejml-fdense-0.46.1.jar:$J/org/ejml/ejml-simple/0.46.1/ejml-simple-0.46.1.jar"
    JCP="$JCP:$J/com/github/wendykierp/JTransforms/3.1/JTransforms-3.1.jar:$J/pl/edu/icm/JLargeArrays/1.5/JLargeArrays-1.5.jar:$J/org/apache/commons/commons-math3/3.6.1/commons-math3-3.6.1.jar"
    JCP="$JCP:$J/org/joml/joml/1.10.8/joml-1.10.8.jar:$J/org/jetbrains/bio/npy/0.3.5/npy-0.3.5.jar:$J/org/jetbrains/kotlin/kotlin-stdlib/1.4.21/kotlin-stdlib-1.4.21.jar"
    missing=""
    for f in ${JCP//:/ }; do [[ -f "$f" ]] || missing="$missing $(basename "$f")"; done
    if ! command -v javac >/dev/null 2>&1; then
        skip_lang java ejml "javac not installed" $JAVA_IDS
    elif [[ -n "$missing" ]]; then
        skip_lang java ejml "pinned jars missing:$missing (run competitors/setup.sh)" $JAVA_IDS
    else
        OUT="$BUILD/java-math-numeric"; SRC="$DIR/java/math/numeric/NumericBench.java"
        if [[ ! -f "$OUT/NumericBench.class" || "$SRC" -nt "$OUT/NumericBench.class" ]]; then
            mkdir -p "$OUT"; javac -cp "$JCP" -d "$OUT" "$SRC" >"$LOGD/cajeta-math-numeric-java-build.log" 2>&1 || rm -f "$OUT/NumericBench.class"
        fi
        if [[ -f "$OUT/NumericBench.class" ]]; then
            PROFILE_LANG_VERSION="$(java -version 2>&1 | head -1 | strip)" java -Xmx8g -cp "$OUT:$JCP" NumericBench 2>"$LOGD/cajeta-math-numeric-java.log" \
                || skip_lang java ejml "java numeric runner crashed (see $LOGD/cajeta-math-numeric-java.log)" $JAVA_IDS
        else
            skip_lang java ejml "javac build failed (see $LOGD/cajeta-math-numeric-java-build.log)" $JAVA_IDS
        fi
    fi
fi
exit 0
