#!/usr/bin/env bash
# process/ competitors (benchmark-reference unit 13, spec §6.12; bench/process/Command.md,
# Process.md): one program per language under competitors/<lang>/process/process/, each running
# process/command-spawn-wait (/bin/true spawned and waited 1e3 times) and process/process-pipe-100mb
# (100 MiB through `cat`) with the API the pages name and the Cajeta side's sizes and checks:
#   cpp     posix_spawn + waitpid, a writer thread on the stdin pipe
#   rust    std::process::Command (status(); piped stdio, writer thread)
#   go      os/exec (Run(); Stdin = bytes.Reader, Output())
#   java    java.lang.ProcessBuilder (start().waitFor(); writer thread, readAllBytes)
#   python  subprocess.run (input=, stdout=PIPE: communicate)
# Standard libraries only. A missing toolchain or a failed build is a `skipped` row per id.
# CAJETA_BENCH_SMOKE=1 selects the smoke sizes (1e2 spawns, 10 MiB). Rows are v1; run.sh lifts them.
set -uo pipefail

DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )/../.." &> /dev/null && pwd )"   # competitors/
BUILD="${DIR}/.build"; mkdir -p "$BUILD"
LOGD="${TMPDIR:-/tmp}"
LANGS="${PROFILE_LANGS:-cpp rust go java python}"
IDS="command-spawn-wait process-pipe-100mb"
strip() { tr -d ',"'; }
[[ -d "$HOME/.cargo/bin" ]] && export PATH="$HOME/.cargo/bin:$PATH"
want() { case " $LANGS " in *" $1 "*) return 0;; *) return 1;; esac; }
skip_lang() {   # skip_lang <lang> <lib> <reason>
    local b
    for b in $IDS; do
        echo "1,${PROFILE_RUN_ID:-local},${PROFILE_RUN_TS:-},${b},process,,0,,0,$1,,$2,,,0,0,-1,-1,-1,-1,,,-1,-1,-1,-1,-1,skipped,,$3,"
    done
}

if want cpp; then
    SRC="$DIR/cpp/process/process/process_bench.cpp"; BIN="$BUILD/cpp-process"
    if ! command -v g++ >/dev/null 2>&1; then skip_lang cpp posix_spawn "g++ not installed"
    else
        if [[ ! -x "$BIN" || "$SRC" -nt "$BIN" ]]; then
            g++ -std=c++17 -O3 -march=native -DNDEBUG -pthread "$SRC" -o "$BIN" >"$LOGD/cajeta-process-cpp-build.log" 2>&1 || rm -f "$BIN"
        fi
        if [[ -x "$BIN" ]]; then
            PROFILE_LANG_VERSION="$(g++ --version | head -1 | strip)" "$BIN" 2>"$LOGD/cajeta-process-cpp.log" \
                || skip_lang cpp posix_spawn "cpp process runner crashed (see $LOGD/cajeta-process-cpp.log)"
        else skip_lang cpp posix_spawn "g++ build failed (see $LOGD/cajeta-process-cpp-build.log)"; fi
    fi
fi

if want rust; then
    RD="$DIR/rust/process/process"
    if ! command -v cargo >/dev/null 2>&1; then skip_lang rust std::process "cargo not installed"
    elif ( cd "$RD" && RUSTFLAGS="-C target-cpu=native" cargo build --release --locked >"$LOGD/cajeta-process-rust-build.log" 2>&1 ); then
        PROFILE_LANG_VERSION="$(rustc --version | strip)" "$RD/target/release/process-bench" 2>"$LOGD/cajeta-process-rust.log" \
            || skip_lang rust std::process "rust process runner crashed (see $LOGD/cajeta-process-rust.log)"
    else skip_lang rust std::process "cargo build failed (see $LOGD/cajeta-process-rust-build.log)"; fi
fi

if want go; then
    GD="$DIR/go/process/process"; GBIN="$BUILD/go-process"
    if ! command -v go >/dev/null 2>&1; then skip_lang go os/exec "go not installed"
    elif ( cd "$GD" && GOAMD64="${GOAMD64:-v3}" go build -o "$GBIN" . >"$LOGD/cajeta-process-go-build.log" 2>&1 ); then
        PROFILE_LANG_VERSION="$(go version | strip)" "$GBIN" 2>"$LOGD/cajeta-process-go.log" \
            || skip_lang go os/exec "go process runner crashed (see $LOGD/cajeta-process-go.log)"
    else skip_lang go os/exec "go build failed (see $LOGD/cajeta-process-go-build.log)"; fi
fi

if want java; then
    OUT="$BUILD/java-process"; SRC="$DIR/java/process/process/ProcessBench.java"
    if ! command -v javac >/dev/null 2>&1; then skip_lang java java.lang.ProcessBuilder "javac not installed"
    else
        if [[ ! -f "$OUT/ProcessBench.class" || "$SRC" -nt "$OUT/ProcessBench.class" ]]; then
            mkdir -p "$OUT"; javac -d "$OUT" "$SRC" >"$LOGD/cajeta-process-java-build.log" 2>&1 || rm -f "$OUT/ProcessBench.class"
        fi
        if [[ -f "$OUT/ProcessBench.class" ]]; then
            PROFILE_LANG_VERSION="$(java -version 2>&1 | head -1 | strip)" java -Xmx2g -cp "$OUT" ProcessBench 2>"$LOGD/cajeta-process-java.log" \
                || skip_lang java java.lang.ProcessBuilder "java process runner crashed (see $LOGD/cajeta-process-java.log)"
        else skip_lang java java.lang.ProcessBuilder "javac build failed (see $LOGD/cajeta-process-java-build.log)"; fi
    fi
fi

if want python; then
    PY="$DIR/python/.venv/bin/python"; [[ -x "$PY" ]] || PY="$(command -v python3 || true)"
    if [[ -n "$PY" ]]; then
        PROFILE_LANG_VERSION="$("$PY" --version 2>&1 | strip)" "$PY" "$DIR/python/process/process/process_bench.py" 2>"$LOGD/cajeta-process-python.log" \
            || skip_lang python subprocess "python process runner crashed (see $LOGD/cajeta-process-python.log)"
    else skip_lang python subprocess "python3 not installed"; fi
fi
exit 0
