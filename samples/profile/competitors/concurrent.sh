#!/usr/bin/env bash
# Concurrency competitor area runner. Builds + runs each present language's runner
# for atomic-fetchadd (1M single-threaded fetch_add → counter==N) and
# task-spawn-await (20K spawn+await of a trivial task → sum==N*(N+1)/2), the same
# sizes as the Cajeta side, and streams schema-conformant CSV rows — the same
# benchmark names, so they line up in the report's concurrent table.
#
# The task primitive differs by language (the point): rust/cpp use OS threads
# (std::thread), go uses goroutines, python uses asyncio; Cajeta uses stackful
# fibers. python skips atomic-fetchadd (no lock-free atomic in the stdlib).
# Cross-check = the exact closed forms. Missing toolchain ⇒ `skipped` row.
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
    for b in atomic-fetchadd task-spawn-await; do
        sz=1000000; [[ "$b" == "task-spawn-await" ]] && sz=20000
        echo "1,${rid},${ts},${b},concurrent,,${sz},,${sz},${lang},,${lib},,,0,0,-1,-1,-1,-1,,,-1,-1,-1,-1,-1,skipped,,${reason},"
    done
}

want() { case " $LANGS " in *" $1 "*) return 0;; *) return 1;; esac; }

# ---- rust (AtomicI64 + std::thread) ----
if want rust; then
    if command -v cargo >/dev/null 2>&1; then
        if ( cd "$DIR/rust/concurrent" && cargo build --release >/tmp/profile-conc-rust.log 2>&1 ); then
            PROFILE_LANG_VERSION="$(rustc --version | strip)" "$DIR/rust/concurrent/target/release/profile-concurrent" \
                || skip_lang rust rust "rust concurrent runner crashed"
        else
            skip_lang rust rust "cargo build failed (see /tmp/profile-conc-rust.log)"
        fi
    else
        skip_lang rust rust "cargo not installed"
    fi
fi

# ---- cpp (std::atomic + std::thread, -pthread) ----
if want cpp; then
    if command -v g++ >/dev/null 2>&1; then
        BIN="$BUILD/cpp-concurrent"
        if [[ ! -x "$BIN" || "$DIR/cpp/concurrent/concurrent_bench.cpp" -nt "$BIN" ]]; then
            g++ -O3 -march=native -pthread -DNDEBUG "$DIR/cpp/concurrent/concurrent_bench.cpp" \
                -o "$BIN" >/tmp/profile-conc-cpp.log 2>&1 || true
        fi
        if [[ -x "$BIN" ]]; then
            PROFILE_LANG_VERSION="$(g++ --version | head -1 | strip)" "$BIN" \
                || skip_lang cpp cpp "cpp concurrent runner crashed"
        else
            skip_lang cpp cpp "g++ build failed — needs pthread (see /tmp/profile-conc-cpp.log)"
        fi
    else
        skip_lang cpp cpp "g++ not installed"
    fi
fi

# ---- go (atomic.Int64 + goroutines) ----
if want go; then
    if command -v go >/dev/null 2>&1; then
        ( cd "$DIR/go/concurrent" && PROFILE_LANG_VERSION="$(go version | strip)" go run . 2>/tmp/profile-conc-go.log ) \
            || skip_lang go go "go build/run failed (see /tmp/profile-conc-go.log)"
    else
        skip_lang go go "go not installed"
    fi
fi

# ---- python (asyncio; atomic skipped inside the runner) ----
if want python; then
    PY="$DIR/python/.venv/bin/python"
    [[ -x "$PY" ]] || PY="$(command -v python3 || true)"
    if [[ -n "$PY" ]]; then
        PROFILE_LANG_VERSION="$("$PY" --version 2>&1 | strip)" "$PY" "$DIR/python/concurrent/concurrent_bench.py" \
            || skip_lang python python "python concurrent runner crashed"
    else
        skip_lang python python "python3 not installed"
    fi
fi
