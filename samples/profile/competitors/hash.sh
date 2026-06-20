#!/usr/bin/env bash
# Hash (bulk-throughput) competitor area runner. Builds + runs each present
# language's bulk-hash runner over a deterministic 1 MiB buffer and streams
# schema-conformant CSV rows (competitors/columns.txt order) to stdout — the
# same xxhash3/sha256/md5 benchmark names the Cajeta side emits, so they line up
# in the report's hash table.
#
# Libraries: rust (xxhash-rust, sha2, md-5), cpp (xxHash + OpenSSL), go
# (zeebo/xxh3 + crypto/sha256 + crypto/md5), python (xxhash + hashlib). A
# missing toolchain/library is a `skip` row with a reason, never a crash.
# `siphash` is recorded as a skip for competitors: there is no portable
# cross-language SipHash with a matching key (Rust's DefaultHasher randomizes;
# C++/Go/Python have no stdlib SipHash) — so a fair head-to-head isn't possible.
#
# Env: PROFILE_RUN_ID, PROFILE_RUN_TS, PROFILE_WARMUP (3), PROFILE_TRIALS (10),
#      PROFILE_LANGS (space-list filter).
set -uo pipefail

DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
BUILD="${DIR}/.build"; mkdir -p "$BUILD"
VENDOR="${DIR}/vendor"
LANGS="${PROFILE_LANGS:-rust cpp go python java}"
strip() { tr -d ',"'; }
N=1048576
BENCHES="xxhash3 sha256 md5"

skip_lang() {
    local lang="$1" lib="$2" reason="$3"
    local rid="${PROFILE_RUN_ID:-local}" ts="${PROFILE_RUN_TS:-}" b
    for b in $BENCHES; do
        echo "1,${rid},${ts},${b},hash,,${N},,${N},${lang},,${lib},,,0,0,-1,-1,-1,-1,,,-1,-1,-1,-1,-1,skipped,,${reason},"
    done
}

# siphash isn't portably comparable across languages — record one skip per lang.
siphash_skip() {
    local rid="${PROFILE_RUN_ID:-local}" ts="${PROFILE_RUN_TS:-}" lang
    for lang in $LANGS; do
        echo "1,${rid},${ts},siphash,hash,,${N},,${N},${lang},,siphash,,,0,0,-1,-1,-1,-1,,,-1,-1,-1,-1,-1,skipped,,no portable cross-language SipHash with a matching key,"
    done
}

want() { case " $LANGS " in *" $1 "*) return 0;; *) return 1;; esac; }

# ---- rust ----
if want rust; then
    if command -v cargo >/dev/null 2>&1; then
        if ( cd "$DIR/rust/hash" && cargo build --release >/tmp/profile-hash-rust.log 2>&1 ); then
            PROFILE_LANG_VERSION="$(rustc --version | strip)" "$DIR/rust/hash/target/release/profile-hash" \
                || skip_lang rust rust "rust hash runner crashed"
        else
            skip_lang rust rust "cargo build failed (see /tmp/profile-hash-rust.log)"
        fi
    else
        skip_lang rust rust "cargo not installed"
    fi
fi

# ---- cpp: xxHash header + OpenSSL libcrypto ----
if want cpp; then
    if command -v g++ >/dev/null 2>&1 && [[ -f "$VENDOR/xxhash/xxhash.h" ]]; then
        BIN="$BUILD/cpp-hash"
        if [[ ! -x "$BIN" || "$DIR/cpp/hash/hash_bulk.cpp" -nt "$BIN" ]]; then
            g++ -O3 -march=native -DNDEBUG -I "$VENDOR/xxhash" \
                "$DIR/cpp/hash/hash_bulk.cpp" -lcrypto -o "$BIN" >/tmp/profile-hash-cpp.log 2>&1 || true
        fi
        if [[ -x "$BIN" ]]; then
            PROFILE_LANG_VERSION="$(g++ --version | head -1 | strip)" "$BIN" \
                || skip_lang cpp cpp "cpp hash runner crashed"
        else
            skip_lang cpp cpp "g++ build failed — needs libcrypto/OpenSSL (see /tmp/profile-hash-cpp.log)"
        fi
    else
        skip_lang cpp cpp "g++ or vendored xxHash missing (run vendor.sh xxhash; needs OpenSSL)"
    fi
fi

# ---- go ----
if want go; then
    if command -v go >/dev/null 2>&1; then
        ( cd "$DIR/go/hash" && go mod tidy >/tmp/profile-hash-go.log 2>&1 && \
          PROFILE_LANG_VERSION="$(go version | strip)" go run . 2>>/tmp/profile-hash-go.log ) \
            || skip_lang go go "go build/run failed (see /tmp/profile-hash-go.log)"
    else
        skip_lang go go "go not installed"
    fi
fi

# ---- python ----
if want python; then
    PY="$DIR/python/.venv/bin/python"
    [[ -x "$PY" ]] || PY="$(command -v python3 || true)"
    if [[ -n "$PY" ]]; then
        PROFILE_LANG_VERSION="$("$PY" --version 2>&1 | strip)" "$PY" "$DIR/python/hash/hash_bulk.py" \
            || skip_lang python python "python hash runner crashed"
    else
        skip_lang python python "python3 not installed"
    fi
fi

# ---- java: sha256 + md5 via java.security.MessageDigest (JDK stdlib); xxhash3 skipped (no JDK xxHash) ----
if want java; then
    if command -v javac >/dev/null 2>&1; then
        OUT="$BUILD/java-hash"; SRC="$DIR/java/hash/HashBench.java"
        if [[ ! -f "$OUT/HashBench.class" || "$SRC" -nt "$OUT/HashBench.class" ]]; then
            mkdir -p "$OUT"; javac -d "$OUT" "$SRC" >/tmp/profile-hash-java.log 2>&1 || true
        fi
        if [[ -f "$OUT/HashBench.class" ]]; then
            PROFILE_LANG_VERSION="$(java -version 2>&1 | head -1 | strip)" java -cp "$OUT" HashBench \
                || skip_lang java java "java hash runner crashed"
        else
            skip_lang java java "javac build failed (see /tmp/profile-hash-java.log)"
        fi
    else
        skip_lang java java "javac not installed"
    fi
fi

siphash_skip
