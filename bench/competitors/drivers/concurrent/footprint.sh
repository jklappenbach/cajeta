#!/usr/bin/env bash
# concurrent/ competitors, program `footprint`: concurrent/tasks-1m-footprint
# (Tasks.md). Each language parks 1e6 tasks on a channel in its own process and
# reports peak_rss_kb (the row's metric) with the RSS growth while parked in
# notes: Rust tokio (watch channel), Go goroutines (chan), Java virtual threads
# (LinkedBlockingQueue), Python asyncio (asyncio.Queue). C++ is a skipped row:
# the page lists no C++ competitor (the standard library has no green task, and
# 1e6 OS threads is not the comparison). Each runner skips itself under 16 GB
# MemAvailable. CAJETA_BENCH_SMOKE=1 parks 1e4.
#
# Env: PROFILE_RUN_ID, PROFILE_RUN_TS, PROFILE_LANGS, CAJETA_BENCH_SMOKE.
set -uo pipefail

DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )/../.." &> /dev/null && pwd )"   # competitors/
BUILD="${DIR}/.build"; mkdir -p "$BUILD"
LOGS="${DIR}/logs"; mkdir -p "$LOGS"
LANGS="${PROFILE_LANGS:-rust cpp go python java}"
[[ -d "$HOME/.cargo/bin" ]] && PATH="$HOME/.cargo/bin:$PATH"
strip() { tr -d ',"'; }
want() { case " $LANGS " in *" $1 "*) return 0;; *) return 1;; esac; }
skip() {  # lang lib reason
    echo "1,${PROFILE_RUN_ID:-local},${PROFILE_RUN_TS:-},tasks-1m-footprint,concurrent,,1000000,,1000000,$1,,$2,,,0,0,-1,-1,-1,-1,,,-1,-1,-1,-1,-1,skipped,,$3,"
}

if want rust; then
    if command -v cargo >/dev/null 2>&1; then
        if ( cd "$DIR/rust/concurrent/footprint" && cargo build --release --offline >"$LOGS/footprint-rust.log" 2>&1 \
                || cargo build --release >>"$LOGS/footprint-rust.log" 2>&1 ); then
            PROFILE_LANG_VERSION="$(rustc --version | strip)" "$DIR/rust/concurrent/footprint/target/release/profile-footprint" \
                || skip rust tokio "rust footprint runner crashed"
        else
            skip rust tokio "cargo build failed (see competitors/logs/footprint-rust.log)"
        fi
    else
        skip rust tokio "cargo not installed"
    fi
fi

want cpp && skip cpp "" "Tasks.md lists no C++ competitor for the footprint (no green task in the standard library; 1e6 OS threads is not the comparison)"

if want go; then
    if command -v go >/dev/null 2>&1; then
        BIN="$BUILD/go-footprint"
        if ( cd "$DIR/go/concurrent/footprint" && GOAMD64="${GOAMD64:-v3}" go build -o "$BIN" . 2>"$LOGS/footprint-go.log" ); then
            PROFILE_LANG_VERSION="$(go version | strip)" "$BIN" || skip go goroutine "go footprint runner crashed"
        else
            skip go goroutine "go build failed (see competitors/logs/footprint-go.log)"
        fi
    else
        skip go goroutine "go not installed"
    fi
fi

if want python; then
    PY="$DIR/python/.venv/bin/python"
    [[ -x "$PY" ]] || PY="$(command -v python3 || true)"
    if [[ -n "$PY" ]]; then
        PROFILE_LANG_VERSION="$("$PY" --version 2>&1 | strip)" "$PY" "$DIR/python/concurrent/footprint/footprint_bench.py" \
            || skip python asyncio "python footprint runner crashed"
    else
        skip python asyncio "python3 not installed"
    fi
fi

if want java; then
    if command -v javac >/dev/null 2>&1; then
        OUT="$BUILD/java-footprint"; SRC="$DIR/java/concurrent/footprint/FootprintBench.java"
        if [[ ! -f "$OUT/FootprintBench.class" || "$SRC" -nt "$OUT/FootprintBench.class" ]]; then
            mkdir -p "$OUT"; javac -d "$OUT" "$SRC" >"$LOGS/footprint-java.log" 2>&1 || rm -f "$OUT/FootprintBench.class"
        fi
        if [[ -f "$OUT/FootprintBench.class" ]]; then
            PROFILE_LANG_VERSION="$(java -version 2>&1 | head -1 | strip)" java -cp "$OUT" FootprintBench \
                || skip java virtual-threads "java footprint runner crashed"
        else
            skip java virtual-threads "javac build failed (see competitors/logs/footprint-java.log)"
        fi
    else
        skip java virtual-threads "javac not installed"
    fi
fi
