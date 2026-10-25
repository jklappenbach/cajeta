#!/usr/bin/env bash
# Shared build-and-run steps for the collection/ drivers added in unit 6
# (drivers/collection/{lists,maps,ordered,cache,ltm}.sh). A driver sets PROG
# (the program directory name under competitors/<lang>/collection/), JAVA_MAIN,
# JAVA_JARS (pins.tsv item names on the classpath), IDS ("id:size ..." for the
# skip rows) and LANGS_SUPPORTED, sources this file and calls coll_run_all.
#
# Each language builds into competitors/.build (gitignored) and logs there; a
# missing toolchain or library, or a failed build, is one skipped row per id
# with the reason, never a silent gap. Pinned C++ headers and Java jars come
# from competitors/third_party (competitors/setup.sh, pins.tsv); the C++
# headers fall back to competitors/vendor/<name> (vendor.sh) when present there.
#
# Env: PROFILE_RUN_ID, PROFILE_RUN_TS, PROFILE_WARMUP, PROFILE_TRIALS,
#      PROFILE_LANGS (space list), PROFILE_BENCH (space list of ids).
set -uo pipefail

DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )/.." &> /dev/null && pwd )"   # competitors/
BUILD="$DIR/.build"; mkdir -p "$BUILD/logs"
TP="$DIR/third_party"
LANGS="${PROFILE_LANGS:-rust cpp go python java}"
export PATH="$HOME/.cargo/bin:$PATH"
strip() { tr -d ',"'; }
want() { case " $LANGS " in *" $1 "*) return 0;; *) return 1;; esac; }

# The first of third_party/<name> and vendor/<name> that holds <rel>.
pinned_path() {
    local name="$1" rel="$2" d
    for d in "$TP/$name" "$DIR/vendor/$name"; do
        [[ -e "$d/$rel" ]] && { printf '%s' "$d"; return 0; }
    done
    return 1
}

skip_lang() {  # lang lib reason
    local lang="$1" lib="$2" reason="$3" rid="${PROFILE_RUN_ID:-local}" ts="${PROFILE_RUN_TS:-}" e id sz
    reason="${reason//,/ }"
    for e in $IDS; do
        id="${e%%:*}"; sz="${e##*:}"
        if [[ -n "${PROFILE_BENCH:-}" ]]; then case " $PROFILE_BENCH " in *" $id "*) ;; *) continue ;; esac; fi
        echo "1,${rid},${ts},${id},collection,,${sz},,${sz},${lang},,${lib},,,0,0,-1,-1,-1,-1,,,-1,-1,-1,-1,-1,skipped,,${reason},"
    done
}

supported() { case " $LANGS_SUPPORTED " in *" $1 "*) return 0;; *) return 1;; esac; }

coll_rust() {
    local src="$DIR/rust/collection/$PROG" log="$BUILD/logs/rust-collection-$PROG.log" d
    command -v cargo >/dev/null 2>&1 || { skip_lang rust rust "cargo not installed (competitors/setup.sh)"; return; }
    if [[ -z "${LIBCLANG_PATH:-}" ]]; then   # bindgen (librocksdb-sys) needs a libclang
        for d in "$DIR/../../../cajeta-llvm/llvm-install/lib" /usr/lib/llvm-*/lib; do
            compgen -G "$d/libclang.so*" >/dev/null && { export LIBCLANG_PATH="$d"; break; }
        done
    fi
    if ( cd "$src" && cargo build --release ) >"$log" 2>&1; then
        PROFILE_LANG_VERSION="$(rustc --version | strip)" "$src/target/release/collection-$PROG" \
            || echo "coll: rust $PROG exited non-zero" >&2
    else
        skip_lang rust rust "cargo build failed (see ${log#$DIR/})"
    fi
}

coll_cpp() {
    local src="$DIR/cpp/collection/$PROG/${PROG}_bench.cpp" bin="$BUILD/cpp-collection-$PROG" log="$BUILD/logs/cpp-collection-$PROG.log" ud pd
    command -v g++ >/dev/null 2>&1 || { skip_lang cpp cpp "g++ not installed"; return; }
    ud="$(pinned_path unordered_dense include/ankerl/unordered_dense.h)" \
        || { skip_lang cpp cpp "pinned unordered_dense missing under competitors/third_party (run competitors/setup.sh)"; return; }
    pd="$(pinned_path pdqsort pdqsort.h)" \
        || { skip_lang cpp cpp "pinned pdqsort missing under competitors/third_party (run competitors/setup.sh)"; return; }
    if [[ ! -x "$bin" || "$src" -nt "$bin" || "$DIR/cpp/collection/common/collgen.h" -nt "$bin" ]]; then
        g++ -O3 -march=native -std=c++20 -DNDEBUG -I "$ud/include" -I "$pd" "$src" -o "$bin.new" >"$log" 2>&1 && mv -f "$bin.new" "$bin"
    fi
    [[ -x "$bin" && ! "$src" -nt "$bin" ]] || { skip_lang cpp cpp "g++ build failed (see ${log#$DIR/})"; return; }
    PROFILE_LANG_VERSION="$(g++ --version | head -1 | strip)" "$bin" || echo "coll: cpp $PROG exited non-zero" >&2
}

coll_go() {
    local log="$BUILD/logs/go-collection-$PROG.log"
    command -v go >/dev/null 2>&1 || { skip_lang go go "go not installed"; return; }
    ( cd "$DIR/go/collection/$PROG" && GOFLAGS=-mod=mod PROFILE_LANG_VERSION="$(go version | strip)" go run . 2>"$log" ) \
        || skip_lang go go "go build/run failed (see ${log#$DIR/})"
}

coll_python() {
    local py="$DIR/python/.venv/bin/python"
    [[ -x "$py" ]] || py="$(command -v python3 || true)"
    [[ -n "$py" ]] || { skip_lang python python "python3 not installed"; return; }
    PROFILE_LANG_VERSION="$("$py" --version 2>&1 | strip)" "$py" "$DIR/python/collection/$PROG/${PROG}_bench.py" \
        || echo "coll: python $PROG exited non-zero" >&2
}

coll_java() {
    local out="$BUILD/java-collection-$PROG" log="$BUILD/logs/java-collection-$PROG.log" cp="" j f stamp
    command -v javac >/dev/null 2>&1 || { skip_lang java java "javac not installed"; return; }
    for j in ${JAVA_JARS:-}; do
        f="$TP/java/$(awk -F'\t' -v n="$j" '!/^#/ && $1==n {print $4; exit}' "$DIR/pins.tsv")"
        [[ -f "$f" ]] || { skip_lang java java "pinned jar $j missing under competitors/third_party/java (run competitors/setup.sh)"; return; }
        cp+="$f:"
    done
    local srcs=("$DIR/java/collection/common/CollGen.java" "$DIR/java/collection/$PROG/$JAVA_MAIN.java")
    stamp="$out/$JAVA_MAIN.class"
    if [[ ! -f "$stamp" || "${srcs[0]}" -nt "$stamp" || "${srcs[1]}" -nt "$stamp" ]]; then
        mkdir -p "$out"; javac -cp "${cp:-.}" -d "$out" "${srcs[@]}" >"$log" 2>&1
    fi
    [[ -f "$stamp" && ! "${srcs[1]}" -nt "$stamp" ]] || { skip_lang java java "javac build failed (see ${log#$DIR/})"; return; }
    PROFILE_LANG_VERSION="$(java -version 2>&1 | head -1 | strip)" java --enable-native-access=ALL-UNNAMED \
        --add-opens java.base/java.nio=ALL-UNNAMED --add-opens java.base/sun.nio.ch=ALL-UNNAMED \
        -cp "$cp$out" "$JAVA_MAIN" 2>>"$log" || echo "coll: java $PROG exited non-zero" >&2
}

coll_run_all() {
    local l
    for l in rust cpp go python java; do
        want "$l" || continue
        if supported "$l"; then "coll_$l"; else skip_lang "$l" "$l" "$UNSUPPORTED_REASON"; fi
    done
}
