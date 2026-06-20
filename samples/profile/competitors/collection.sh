#!/usr/bin/env bash
# Collection competitor area runner. Builds + runs each present language's
# collection runner (hashmap-int / hashmap-string / hashset-dedup /
# arraylist-append, same sizes + init as the Cajeta side) and streams schema-
# conformant CSV rows to stdout — the same benchmark names, so they line up in
# the report's collection table. The hashmap benchmarks follow the Ankerl
# insert+find shape.
#
# Libraries: rust (std HashMap + ahash, HashSet, Vec), cpp (std::unordered_map +
# ankerl::unordered_dense, std::unordered_set, std::vector), go (builtin map /
# slice), python (dict / set / list). Missing toolchain/lib ⇒ `skipped` row.
#
# Env: PROFILE_RUN_ID, PROFILE_RUN_TS, PROFILE_WARMUP (3), PROFILE_TRIALS (10),
#      PROFILE_LANGS (space-list filter).
set -uo pipefail

DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
BUILD="${DIR}/.build"; mkdir -p "$BUILD"
VENDOR="${DIR}/vendor"
LANGS="${PROFILE_LANGS:-rust cpp go python}"
strip() { tr -d ',"'; }

skip_lang() {
    local lang="$1" lib="$2" reason="$3"
    local rid="${PROFILE_RUN_ID:-local}" ts="${PROFILE_RUN_TS:-}" b sz
    for b in hashmap-int hashmap-string hashset-dedup arraylist-append; do
        case "$b" in hashmap-int) sz=50000;; hashmap-string) sz=30000;; *) sz=100000;; esac
        echo "1,${rid},${ts},${b},collection,,${sz},,${sz},${lang},,${lib},,,0,0,-1,-1,-1,-1,,,-1,-1,-1,-1,-1,skipped,,${reason},"
    done
}

want() { case " $LANGS " in *" $1 "*) return 0;; *) return 1;; esac; }

# ---- rust (std + ahash) ----
if want rust; then
    if command -v cargo >/dev/null 2>&1; then
        if ( cd "$DIR/rust/collection" && cargo build --release >/tmp/profile-coll-rust.log 2>&1 ); then
            PROFILE_LANG_VERSION="$(rustc --version | strip)" "$DIR/rust/collection/target/release/profile-collection" \
                || skip_lang rust rust "rust collection runner crashed"
        else
            skip_lang rust rust "cargo build failed (see /tmp/profile-coll-rust.log)"
        fi
    else
        skip_lang rust rust "cargo not installed"
    fi
fi

# ---- cpp (std + ankerl::unordered_dense) ----
if want cpp; then
    if command -v g++ >/dev/null 2>&1 && [[ -f "$VENDOR/unordered_dense/include/ankerl/unordered_dense.h" ]]; then
        BIN="$BUILD/cpp-collection"
        if [[ ! -x "$BIN" || "$DIR/cpp/collection/collection_bench.cpp" -nt "$BIN" ]]; then
            g++ -O3 -march=native -std=c++17 -DNDEBUG -I "$VENDOR/unordered_dense/include" \
                "$DIR/cpp/collection/collection_bench.cpp" -o "$BIN" >/tmp/profile-coll-cpp.log 2>&1 || true
        fi
        if [[ -x "$BIN" ]]; then
            PROFILE_LANG_VERSION="$(g++ --version | head -1 | strip)" "$BIN" \
                || skip_lang cpp cpp "cpp collection runner crashed"
        else
            skip_lang cpp cpp "g++ build failed (see /tmp/profile-coll-cpp.log)"
        fi
    else
        skip_lang cpp cpp "g++ or vendored unordered_dense missing (run vendor.sh unordered_dense)"
    fi
fi

# ---- go ----
if want go; then
    if command -v go >/dev/null 2>&1; then
        ( cd "$DIR/go/collection" && PROFILE_LANG_VERSION="$(go version | strip)" go run . 2>/tmp/profile-coll-go.log ) \
            || skip_lang go go "go build/run failed (see /tmp/profile-coll-go.log)"
    else
        skip_lang go go "go not installed"
    fi
fi

# ---- python ----
if want python; then
    PY="$DIR/python/.venv/bin/python"
    [[ -x "$PY" ]] || PY="$(command -v python3 || true)"
    if [[ -n "$PY" ]]; then
        PROFILE_LANG_VERSION="$("$PY" --version 2>&1 | strip)" "$PY" "$DIR/python/collection/collection_bench.py" \
            || skip_lang python python "python collection runner crashed"
    else
        skip_lang python python "python3 not installed"
    fi
fi
