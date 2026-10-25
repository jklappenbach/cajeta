#!/usr/bin/env bash
# concurrent/ competitors, program `concurrent`: every id on bench/concurrent/*.md
# except tasks-1m-footprint (its own program, drivers/concurrent/footprint.sh, so
# its peak RSS is its own process's). Builds and runs each present language's
# runner and streams v1 rows (lib/rows.sh lifts them to v2):
#   cpp     std::atomic, std::mutex, std::shared_mutex, std::counting_semaphore,
#           thread_local, std::async; moodycamel::BlockingConcurrentQueue
#           (third_party/concurrentqueue, pins.tsv) for the channel rows
#   rust    std atomics/Mutex/RwLock/thread_local!/mpsc, tokio 1.53.1
#           (Semaphore, sleep, spawn), crossbeam-channel 0.5.16
#   go      sync/atomic, sync.Mutex/RWMutex, chan, time.Sleep, context, goroutines
#   java    java.util.concurrent, ScopedValue, ThreadLocal, virtual threads
#   python  threading, itertools.count, contextvars, asyncio (GIL noted)
# The contended rows use OS threads (Go: goroutines) where Cajeta uses fibers.
# CAJETA_BENCH_SMOKE=1 shrinks the sizes the same way the Cajeta side does.
# A missing toolchain or a failed build yields skipped rows with the reason.
#
# Env: PROFILE_RUN_ID, PROFILE_RUN_TS, PROFILE_WARMUP (3), PROFILE_TRIALS (10),
#      PROFILE_LANGS (space-list filter), CAJETA_BENCH_SMOKE.
set -uo pipefail

DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )/../.." &> /dev/null && pwd )"   # competitors/
BUILD="${DIR}/.build"; mkdir -p "$BUILD"
LOGS="${DIR}/logs"; mkdir -p "$LOGS"
LANGS="${PROFILE_LANGS:-rust cpp go python java}"
[[ -d "$HOME/.cargo/bin" ]] && PATH="$HOME/.cargo/bin:$PATH"
strip() { tr -d ',"'; }

IDS="atomic-fetchadd atomic-fetchadd-32 atomic-cas atomic-contended-16 mutex-uncontended
mutex-contended-4 mutex-contended-16 rwlock-read-heavy lock-uncontended semaphore-acquire-release
channel-spsc channel-mpsc channel-mpmc fiber-sleep-wake fiberlocal-get-set task-spawn-await"

skip_lang() {
    local lang="$1" reason="$2" rid="${PROFILE_RUN_ID:-local}" ts="${PROFILE_RUN_TS:-}" b
    for b in $IDS; do
        echo "1,${rid},${ts},${b},concurrent,,-1,,-1,${lang},,,,,0,0,-1,-1,-1,-1,,,-1,-1,-1,-1,-1,skipped,,${reason},"
    done
}

want() { case " $LANGS " in *" $1 "*) return 0;; *) return 1;; esac; }

# ---- rust ----
if want rust; then
    if command -v cargo >/dev/null 2>&1; then
        if ( cd "$DIR/rust/concurrent/concurrent" && cargo build --release --offline >"$LOGS/conc-rust.log" 2>&1 \
                || cargo build --release >>"$LOGS/conc-rust.log" 2>&1 ); then
            PROFILE_LANG_VERSION="$(rustc --version | strip)" "$DIR/rust/concurrent/concurrent/target/release/profile-concurrent" \
                || skip_lang rust "rust concurrent runner crashed"
        else
            skip_lang rust "cargo build failed (see competitors/logs/conc-rust.log)"
        fi
    else
        skip_lang rust "cargo not installed"
    fi
fi

# ---- cpp ----
if want cpp; then
    CQ="$DIR/third_party/concurrentqueue"
    if ! command -v g++ >/dev/null 2>&1; then
        skip_lang cpp "g++ not installed"
    elif [[ ! -f "$CQ/blockingconcurrentqueue.h" ]]; then
        skip_lang cpp "moodycamel concurrentqueue missing (run competitors/setup.sh)"
    else
        BIN="$BUILD/cpp-concurrent"; SRC="$DIR/cpp/concurrent/concurrent/concurrent_bench.cpp"
        if [[ ! -x "$BIN" || "$SRC" -nt "$BIN" ]]; then
            g++ -std=c++20 -O3 -march=native -pthread -DNDEBUG -I"$CQ" "$SRC" -o "$BIN" >"$LOGS/conc-cpp.log" 2>&1 || rm -f "$BIN"
        fi
        if [[ -x "$BIN" ]]; then
            PROFILE_LANG_VERSION="$(g++ --version | head -1 | strip)" "$BIN" || skip_lang cpp "cpp concurrent runner crashed"
        else
            skip_lang cpp "g++ build failed (see competitors/logs/conc-cpp.log)"
        fi
    fi
fi

# ---- go ----
if want go; then
    if command -v go >/dev/null 2>&1; then
        BIN="$BUILD/go-concurrent"
        if ( cd "$DIR/go/concurrent/concurrent" && GOAMD64="${GOAMD64:-v3}" go build -o "$BIN" . 2>"$LOGS/conc-go.log" ); then
            PROFILE_LANG_VERSION="$(go version | strip)" "$BIN" || skip_lang go "go concurrent runner crashed"
        else
            skip_lang go "go build failed (see competitors/logs/conc-go.log)"
        fi
    else
        skip_lang go "go not installed"
    fi
fi

# ---- python ----
if want python; then
    PY="$DIR/python/.venv/bin/python"
    [[ -x "$PY" ]] || PY="$(command -v python3 || true)"
    if [[ -n "$PY" ]]; then
        PROFILE_LANG_VERSION="$("$PY" --version 2>&1 | strip)" "$PY" "$DIR/python/concurrent/concurrent/concurrent_bench.py" \
            || skip_lang python "python concurrent runner crashed"
    else
        skip_lang python "python3 not installed"
    fi
fi

# ---- java ----
if want java; then
    if command -v javac >/dev/null 2>&1; then
        OUT="$BUILD/java-concurrent"; SRC="$DIR/java/concurrent/concurrent/ConcurrentBench.java"
        if [[ ! -f "$OUT/ConcurrentBench.class" || "$SRC" -nt "$OUT/ConcurrentBench.class" ]]; then
            mkdir -p "$OUT"; javac -d "$OUT" "$SRC" >"$LOGS/conc-java.log" 2>&1 || rm -f "$OUT/ConcurrentBench.class"
        fi
        if [[ -f "$OUT/ConcurrentBench.class" ]]; then
            PROFILE_LANG_VERSION="$(java -version 2>&1 | head -1 | strip)" java -cp "$OUT" ConcurrentBench \
                || skip_lang java "java concurrent runner crashed"
        else
            skip_lang java "javac build failed (see competitors/logs/conc-java.log)"
        fi
    else
        skip_lang java "javac not installed"
    fi
fi
