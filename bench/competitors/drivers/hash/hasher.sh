#!/usr/bin/env bash
# hash/ competitor program `hasher` (benchmark-reference unit 8): the ids of
# bench/hash/DefaultHasher.md and Hash.md, same inputs and folds as
# bench/src/main/cajeta/bench/hash/:
#   defaulthasher-string (1e7, 1024 16-byte keys), defaulthasher-int (1e8),
#   hash-struct (1e7, 4096 three-field structs).
# Competitors as the pages name them: rust std DefaultHasher and
# #[derive(Hash)]; go hash/maphash; java String.hashCode's polynomial
# (Arrays.hashCode), Long.hashCode, Objects.hash; python hash(); cpp
# boost::hash_combine (header-only Boost.ContainerHash 1.89, third_party/boost-*,
# hash-struct only). A missing toolchain or library is a `skipped` row per id.
#
# Env: PROFILE_RUN_ID, PROFILE_RUN_TS, PROFILE_WARMUP (3), PROFILE_TRIALS (10),
#      PROFILE_LANGS (space-list filter).
set -uo pipefail

DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )/../.." &> /dev/null && pwd )"   # competitors/
BUILD="${DIR}/.build"; mkdir -p "$BUILD"
LOGS="${BUILD}/logs"; mkdir -p "$LOGS"
TP="${DIR}/third_party"
LANGS="${PROFILE_LANGS:-cpp rust go java python}"
strip() { tr -d ',"'; }
want() { case " $LANGS " in *" $1 "*) return 0;; *) return 1;; esac; }

skip() {
    local lang="$1" reason="$2" rid="${PROFILE_RUN_ID:-local}" ts="${PROFILE_RUN_TS:-}" spec b sz lib
    shift 2
    for spec in "$@"; do
        IFS=: read -r b sz lib <<<"$spec"
        echo "1,${rid},${ts},${b},hash,,${sz},,${sz},${lang},,${lib},,,0,0,-1,-1,-1,-1,,,-1,-1,-1,-1,-1,skipped,false,${reason},"
    done
}
three() { echo "defaulthasher-string:10000000:$1 defaulthasher-int:100000000:$1 hash-struct:10000000:${2:-$1}"; }

# ---- cpp: boost::hash_combine (hash-struct only) ----
if want cpp; then
    S="$DIR/cpp/hash/hasher/hasher_bench.cpp"; BIN="$BUILD/cpp-hasher"
    BI=(); for b in container_hash config describe mp11; do BI+=(-I "$TP/boost-$b/include"); done
    if ! command -v g++ >/dev/null 2>&1; then
        skip cpp "g++ not installed" hash-struct:10000000:boost::hash_combine
    elif [[ ! -f "$TP/boost-container_hash/include/boost/container_hash/hash.hpp" ]]; then
        skip cpp "third_party/boost-* missing (run competitors/setup.sh)" hash-struct:10000000:boost::hash_combine
    else
        if [[ ! -x "$BIN" || "$S" -nt "$BIN" ]]; then
            g++ -O3 -march=native -std=c++17 -DNDEBUG "${BI[@]}" "$S" -o "$BIN" >"$LOGS/cpp-hasher.log" 2>&1 || rm -f "$BIN"
        fi
        if [[ -x "$BIN" ]]; then
            PROFILE_LANG_VERSION="$(g++ --version | head -1 | strip)" "$BIN" || skip cpp "cpp hasher runner crashed" hash-struct:10000000:boost::hash_combine
        else
            skip cpp "g++ build failed (see .build/logs/cpp-hasher.log)" hash-struct:10000000:boost::hash_combine
        fi
    fi
fi

# ---- rust: std ----
if want rust; then
    if ! command -v cargo >/dev/null 2>&1; then
        skip rust "cargo not installed" $(three std)
    elif ( cd "$DIR/rust/hash/hasher" && cargo build --release >"$LOGS/rust-hasher.log" 2>&1 ); then
        PROFILE_LANG_VERSION="$(rustc --version | strip)" "$DIR/rust/hash/hasher/target/release/bench-hasher" \
            || skip rust "rust hasher runner crashed" $(three std)
    else
        skip rust "cargo build failed (see .build/logs/rust-hasher.log)" $(three std)
    fi
fi

# ---- go: hash/maphash ----
if want go; then
    if command -v go >/dev/null 2>&1; then
        ( cd "$DIR/go/hash/hasher" && GOAMD64="${GOAMD64:-v3}" PROFILE_LANG_VERSION="$(go version | strip)" go run . 2>"$LOGS/go-hasher.log" ) \
            || skip go "go build/run failed (see .build/logs/go-hasher.log)" $(three maphash)
    else
        skip go "go not installed" $(three maphash)
    fi
fi

# ---- java: java.base + java.util.Objects ----
if want java; then
    if command -v javac >/dev/null 2>&1; then
        OUT="$BUILD/java-hasher"; SRC="$DIR/java/hash/hasher/HasherBench.java"
        if [[ ! -f "$OUT/HasherBench.class" || "$SRC" -nt "$OUT/HasherBench.class" ]]; then
            mkdir -p "$OUT"; javac -d "$OUT" "$SRC" >"$LOGS/java-hasher.log" 2>&1 || rm -f "$OUT/HasherBench.class"
        fi
        if [[ -f "$OUT/HasherBench.class" ]]; then
            PROFILE_LANG_VERSION="$(java -version 2>&1 | head -1 | strip)" java -cp "$OUT" HasherBench \
                || skip java "java hasher runner crashed" $(three java.base java.util.Objects)
        else
            skip java "javac build failed (see .build/logs/java-hasher.log)" $(three java.base java.util.Objects)
        fi
    else
        skip java "javac not installed" $(three java.base java.util.Objects)
    fi
fi

# ---- python: builtins hash() (not on Hash.md: no hash-struct row) ----
if want python; then
    PY="$DIR/python/.venv/bin/python"; [[ -x "$PY" ]] || PY="$(command -v python3 || true)"
    if [[ -n "$PY" ]]; then
        PROFILE_LANG_VERSION="$("$PY" --version 2>&1 | strip)" "$PY" "$DIR/python/hash/hasher/hasher_bench.py" \
            || skip python "python hasher runner crashed" defaulthasher-string:10000000:builtins defaulthasher-int:100000000:builtins
    else
        skip python "python3 not installed" defaulthasher-string:10000000:builtins defaulthasher-int:100000000:builtins
    fi
fi
