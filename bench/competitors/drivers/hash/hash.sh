#!/usr/bin/env bash
# hash/ competitor program `hash` (benchmark-reference unit 8): the ids of
# bench/hash/XXHash3.md and SipHash.md in every language the pages name, same
# inputs, folds and reference checks as bench/src/main/cajeta/bench/hash/:
#   xxhash3, xxhash3_128, siphash: 1e7 hashes of 32-byte keys;
#   xxhash3-256k, xxhash3_128-256k: 1e4 hashes of a 256 KB buffer.
# Libraries: cpp xxHash (third_party/xxhash at vendor/xxhash.lock, the pin
# setup.sh fetches; vendor/xxhash, vendor.sh's optional clone, is a fallback)
# and the SipHash reference C (third_party/siphash); rust xxhash-rust and
# siphasher; go zeebo/xxh3 and dchest/siphash; java zero-allocation-hashing
# and guava (third_party/java jars); python xxhash and siphash24. A missing
# toolchain or library is a `skipped` row per id with the reason. Rows are v1;
# run.sh normalizes them.
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

# skip <lang> <reason> <bench:bytes:lib>...
skip() {
    local lang="$1" reason="$2" rid="${PROFILE_RUN_ID:-local}" ts="${PROFILE_RUN_TS:-}" spec b sz lib
    shift 2
    for spec in "$@"; do
        IFS=: read -r b sz lib <<<"$spec"
        echo "1,${rid},${ts},${b},hash,,${sz},,${sz},${lang},,${lib},,,0,0,-1,-1,-1,-1,,,-1,-1,-1,-1,-1,skipped,false,${reason},"
    done
}
specs() { local x="$1" s; shift; for s in xxhash3:320000000 xxhash3_128:320000000 xxhash3-256k:2621440000 xxhash3_128-256k:2621440000; do echo "$s:$x"; done; echo "siphash:320000000:$1"; }

# ---- cpp: xxHash + SipHash reference C ----
if want cpp; then
    XXH=""
    for d in "$TP/xxhash" "$DIR/vendor/xxhash"; do [[ -f "$d/xxhash.h" ]] && { XXH="$d"; break; }; done
    S="$DIR/cpp/hash/hash/hash_bench.cpp"; BIN="$BUILD/cpp-hash"
    if ! command -v g++ >/dev/null 2>&1 || ! command -v gcc >/dev/null 2>&1; then
        skip cpp "g++/gcc not installed" $(specs xxHash siphash-reference)
    elif [[ -z "$XXH" || ! -f "$TP/siphash/siphash.c" ]]; then
        skip cpp "third_party/xxhash or third_party/siphash missing (run competitors/setup.sh)" $(specs xxHash siphash-reference)
    else
        if [[ ! -x "$BIN" || "$S" -nt "$BIN" ]]; then
            { gcc -O3 -march=native -DNDEBUG -c "$TP/siphash/siphash.c" -o "$BUILD/siphash.o" &&
              g++ -O3 -march=native -DNDEBUG -I "$XXH" -I "$TP/siphash" "$S" "$BUILD/siphash.o" -o "$BIN"; } \
                >"$LOGS/cpp-hash.log" 2>&1 || rm -f "$BIN"
        fi
        if [[ -x "$BIN" ]]; then
            PROFILE_LANG_VERSION="$(g++ --version | head -1 | strip)" "$BIN" || skip cpp "cpp hash runner crashed" $(specs xxHash siphash-reference)
        else
            skip cpp "g++ build failed (see .build/logs/cpp-hash.log)" $(specs xxHash siphash-reference)
        fi
    fi
fi

# ---- rust: xxhash-rust + siphasher ----
if want rust; then
    if ! command -v cargo >/dev/null 2>&1; then
        skip rust "cargo not installed" $(specs xxhash-rust siphasher)
    elif ( cd "$DIR/rust/hash/hash" && cargo build --release >"$LOGS/rust-hash.log" 2>&1 ); then
        PROFILE_LANG_VERSION="$(rustc --version | strip)" "$DIR/rust/hash/hash/target/release/profile-hash" \
            || skip rust "rust hash runner crashed" $(specs xxhash-rust siphasher)
    else
        skip rust "cargo build failed (see .build/logs/rust-hash.log)" $(specs xxhash-rust siphasher)
    fi
fi

# ---- go: zeebo/xxh3 + dchest/siphash ----
if want go; then
    if command -v go >/dev/null 2>&1; then
        ( cd "$DIR/go/hash/hash" && go mod download >"$LOGS/go-hash.log" 2>&1 && \
          GOAMD64="${GOAMD64:-v3}" PROFILE_LANG_VERSION="$(go version | strip)" go run . 2>>"$LOGS/go-hash.log" ) \
            || skip go "go build/run failed (see .build/logs/go-hash.log)" $(specs zeebo/xxh3 dchest/siphash)
    else
        skip go "go not installed" $(specs zeebo/xxh3 dchest/siphash)
    fi
fi

# ---- java: zero-allocation-hashing + guava (pinned jars) ----
if want java; then
    J="$TP/java"
    CP="$J/net/openhft/zero-allocation-hashing/2026.0/zero-allocation-hashing-2026.0.jar:$J/com/google/guava/guava/33.7.2-jre/guava-33.7.2-jre.jar"
    if ! command -v javac >/dev/null 2>&1; then
        skip java "javac not installed" $(specs zero-allocation-hashing guava)
    elif [[ ! -f "${CP%%:*}" || ! -f "${CP##*:}" ]]; then
        skip java "pinned jars missing under third_party/java (run competitors/setup.sh)" $(specs zero-allocation-hashing guava)
    else
        OUT="$BUILD/java-hash"; SRC="$DIR/java/hash/hash/HashBench.java"
        if [[ ! -f "$OUT/HashBench.class" || "$SRC" -nt "$OUT/HashBench.class" ]]; then
            mkdir -p "$OUT"; javac -cp "$CP" -d "$OUT" "$SRC" >"$LOGS/java-hash.log" 2>&1 || rm -f "$OUT/HashBench.class"
        fi
        if [[ -f "$OUT/HashBench.class" ]]; then
            PROFILE_LANG_VERSION="$(java -version 2>&1 | head -1 | strip)" java -cp "$OUT:$CP" HashBench 2>>"$LOGS/java-hash.log" \
                || skip java "java hash runner crashed" $(specs zero-allocation-hashing guava)
        else
            skip java "javac build failed (see .build/logs/java-hash.log)" $(specs zero-allocation-hashing guava)
        fi
    fi
fi

# ---- python: xxhash + siphash24 ----
if want python; then
    PY="$DIR/python/.venv/bin/python"
    if [[ ! -x "$PY" ]]; then
        skip python "competitors/python/.venv missing (run competitors/setup.sh)" $(specs xxhash siphash24)
    elif ! "$PY" -c "import xxhash, siphash24" >/dev/null 2>&1; then
        skip python "xxhash or siphash24 not installed in the venv (run competitors/setup.sh)" $(specs xxhash siphash24)
    else
        PROFILE_LANG_VERSION="$("$PY" --version 2>&1 | strip)" "$PY" "$DIR/python/hash/hash/hash_bench.py" \
            || skip python "python hash runner crashed" $(specs xxhash siphash24)
    fi
fi
