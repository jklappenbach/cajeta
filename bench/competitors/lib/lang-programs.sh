#!/usr/bin/env bash
# Shared build-and-run steps for the bench/lang/ competitor programs of
# benchmark-reference unit 5 (drivers/lang/{loops,exceptions,alloc,dispatch,
# text,values,mathscalar,guid}.sh). Each program exists once per language:
#   cpp/lang/<p>/<p>_bench.cpp       g++ -O3 -march=native -std=c++23
#   rust/lang/<p>/ (lang-<p>)         cargo build --release, target-cpu=native
#   go/lang/<p>/                      go build (GOAMD64 from lib/rows.sh)
#   java/lang/<p>/<Class>.java        javac; jvm.flags beside it if any
#   python/lang/<p>/<p>_bench.py      the competitors venv
# A program prints v1 rows (32 columns, area lang) on stdout; run.sh pipes
# them through rows_v2. A missing toolchain or a failed build prints one
# `skipped` row per id the program owns (LANG_SLUGS, "slug[:variant]"), with
# the reason, so every id still has a row in that language.
#
# A driver sets LANG_PROG and LANG_SLUGS, sources this file and calls
# lang_run_all [cpp-extra-link-flags].
#
# Env: PROFILE_RUN_ID, PROFILE_RUN_TS, PROFILE_WARMUP, PROFILE_TRIALS,
# PROFILE_LANGS (space list filter), LANG_ONLY (comma list of slugs),
# PROFILE_PY_DIVISOR (python only; the smoke scales CPython's 1e8 loops).

LANG_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )/.." &> /dev/null && pwd )"   # competitors/
LANG_BUILD="${LANG_DIR}/.build/lang"
LANG_LOGS="${LANG_DIR}/logs"
mkdir -p "$LANG_BUILD" "$LANG_LOGS"
: "${PROFILE_LANGS:=cpp rust go java python}"
# rustup installs user-level into ~/.cargo/bin, which a non-login shell may
# not have on PATH (competitors/toolchains-<machine>.tsv records it there).
if ! command -v cargo >/dev/null 2>&1 && [[ -x "$HOME/.cargo/bin/cargo" ]]; then
    PATH="$HOME/.cargo/bin:$PATH"; export PATH
fi

lang_strip() { tr -d ',"'; }
lang_want() { case " $PROFILE_LANGS " in *" $1 "*) return 0;; *) return 1;; esac; }

lang_selected() {   # slug -> 0 when LANG_ONLY is empty or names it
    [[ -z "${LANG_ONLY:-}" ]] && return 0
    case ",${LANG_ONLY}," in *",$1,"*) return 0;; *) return 1;; esac
}

lang_skip_all() {   # language library reason
    local lang="$1" lib="$2" reason="$3" e slug var
    reason="${reason//,/;}"
    for e in $LANG_SLUGS; do
        slug="${e%%:*}"; var=""; [[ "$e" == *:* ]] && var="${e#*:}"
        lang_selected "$slug" || continue
        echo "1,${PROFILE_RUN_ID:-local},${PROFILE_RUN_TS:-},${slug},lang,,0,,0,${lang},,${lib},,,0,0,-1,-1,-1,-1,,,-1,-1,-1,-1,-1,skipped,,${reason},${var},cpu"
    done
}

lang_run_cpp() {    # extra link flags...
    local p="$LANG_PROG" src="$LANG_DIR/cpp/lang/$LANG_PROG/${LANG_PROG}_bench.cpp" bin="$LANG_BUILD/cpp-$LANG_PROG"
    command -v g++ >/dev/null 2>&1 || { lang_skip_all cpp cpp "g++ not installed"; return; }
    if [[ ! -x "$bin" || "$src" -nt "$bin" ]]; then
        g++ -O3 -march=native -std=c++23 -DNDEBUG "$src" -o "$bin" "$@" >"$LANG_LOGS/lang-$p-cpp.log" 2>&1 \
            || { rm -f "$bin"; lang_skip_all cpp cpp "g++ build failed (see competitors/logs/lang-$p-cpp.log)"; return; }
    fi
    PROFILE_LANG_VERSION="$(g++ --version | head -1 | lang_strip)" "$bin" \
        || lang_skip_all cpp cpp "cpp $p runner exited non-zero"
}

lang_run_rust() {
    local p="$LANG_PROG" d="$LANG_DIR/rust/lang/$LANG_PROG"
    command -v cargo >/dev/null 2>&1 || { lang_skip_all rust rust "cargo not installed"; return; }
    ( cd "$d" && RUSTFLAGS="-C target-cpu=native" cargo build --release >"$LANG_LOGS/lang-$p-rust.log" 2>&1 ) \
        || { lang_skip_all rust rust "cargo build failed (see competitors/logs/lang-$p-rust.log)"; return; }
    PROFILE_LANG_VERSION="$(rustc --version | lang_strip)" "$d/target/release/lang-$p" \
        || lang_skip_all rust rust "rust $p runner exited non-zero"
}

lang_run_go() {
    local p="$LANG_PROG" d="$LANG_DIR/go/lang/$LANG_PROG" bin="$LANG_BUILD/go-$LANG_PROG" ver
    command -v go >/dev/null 2>&1 || { lang_skip_all go go "go not installed"; return; }
    ( cd "$d" && GOAMD64="${GOAMD64:-v3}" go build -o "$bin" . >"$LANG_LOGS/lang-$p-go.log" 2>&1 ) \
        || { lang_skip_all go go "go build failed (see competitors/logs/lang-$p-go.log)"; return; }
    ver="$(go version | lang_strip)"
    PROFILE_LANG_VERSION="$ver" "$bin" || lang_skip_all go go "go $p runner exited non-zero"
    # loops: the bounds-off variant of loop-counted-nobounds is the same
    # program built with the compiler's bounds checks removed (-gcflags=-B).
    if [[ "$p" == "loops" ]] && lang_selected loop-counted-nobounds; then
        ( cd "$d" && GOAMD64="${GOAMD64:-v3}" go build -gcflags=-B -o "$bin-nobounds" . >>"$LANG_LOGS/lang-$p-go.log" 2>&1 ) \
            && PROFILE_LANG_VERSION="$ver" LANG_BOUNDS=off LANG_ONLY=loop-counted-nobounds "$bin-nobounds"
    fi
}

lang_run_java() {   # class
    local p="$LANG_PROG" cls="$1" d="$LANG_DIR/java/lang/$LANG_PROG" out="$LANG_BUILD/java-$LANG_PROG" flags=""
    command -v javac >/dev/null 2>&1 || { lang_skip_all java java "javac not installed"; return; }
    if [[ ! -f "$out/$cls.class" || "$d/$cls.java" -nt "$out/$cls.class" ]]; then
        mkdir -p "$out"
        javac -d "$out" "$d/$cls.java" >"$LANG_LOGS/lang-$p-java.log" 2>&1 \
            || { lang_skip_all java java "javac failed (see competitors/logs/lang-$p-java.log)"; return; }
    fi
    [[ -f "$d/jvm.flags" ]] && flags="$(cat "$d/jvm.flags")"
    # shellcheck disable=SC2086
    PROFILE_LANG_VERSION="$(java -version 2>&1 | head -1 | lang_strip)" java $flags -cp "$out" "$cls" \
        || lang_skip_all java java "java $p runner exited non-zero"
}

lang_run_python() {
    local p="$LANG_PROG" py="$LANG_DIR/python/.venv/bin/python"
    [[ -x "$py" ]] || py="$(command -v python3 || true)"
    [[ -n "$py" ]] || { lang_skip_all python python "python3 not installed"; return; }
    PROFILE_LANG_VERSION="$("$py" --version 2>&1 | lang_strip)" "$py" "$LANG_DIR/python/lang/$p/${p}_bench.py" \
        || lang_skip_all python python "python $p runner exited non-zero"
}

lang_run_all() {    # [cpp extra link flags...]
    lang_want cpp    && lang_run_cpp "$@"
    lang_want rust   && lang_run_rust
    lang_want go     && lang_run_go
    lang_want java   && lang_run_java "$LANG_CLASS"
    lang_want python && lang_run_python
    return 0
}
