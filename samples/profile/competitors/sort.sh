#!/usr/bin/env bash
# Sort competitor area runner. Builds + runs each present language's sort runner
# (50K int64 across random/ascending/descending/dups + a 50K f64 random sort)
# and streams schema-conformant CSV rows (competitors/columns.txt order) to
# stdout — the same sort-int64/sort-f64/sort-stable-int64 benchmark names + the
# same four variants the Cajeta side uses, so they line up in the report's sort
# table. Diagnostic: Cajeta's quicksort overflows its 128-deep stack on the
# adversarial patterns and skips them; these std sorts complete all four.
#
# Libraries: rust (std sort_unstable/sort), cpp (std::sort, vendored pdqsort,
# std::stable_sort), go (slices.Sort/SortStableFunc), python (Timsort). Missing
# toolchain/lib ⇒ `skipped` row with a reason.
#
# Env: PROFILE_RUN_ID, PROFILE_RUN_TS, PROFILE_WARMUP (3), PROFILE_TRIALS (10),
#      PROFILE_LANGS (space-list filter).
set -uo pipefail

DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
BUILD="${DIR}/.build"; mkdir -p "$BUILD"
VENDOR="${DIR}/vendor"
LANGS="${PROFILE_LANGS:-rust cpp go python java}"
strip() { tr -d ',"'; }
N=50000

# One skipped row per (benchmark, variant) a language can't run.
skip_lang() {
    local lang="$1" lib="$2" reason="$3"
    local rid="${PROFILE_RUN_ID:-local}" ts="${PROFILE_RUN_TS:-}" v
    for v in random ascending descending dups; do
        echo "1,${rid},${ts},sort-int64,sort,,${N},,${N},${lang},,${lib},,,0,0,-1,-1,-1,-1,,,-1,-1,-1,-1,-1,skipped,,${reason},${v}"
    done
    echo "1,${rid},${ts},sort-f64,sort,,${N},,${N},${lang},,${lib},,,0,0,-1,-1,-1,-1,,,-1,-1,-1,-1,-1,skipped,,${reason},"
}

want() { case " $LANGS " in *" $1 "*) return 0;; *) return 1;; esac; }

# ---- rust ----
if want rust; then
    if command -v cargo >/dev/null 2>&1; then
        if ( cd "$DIR/rust/sort" && cargo build --release >/tmp/profile-sort-rust.log 2>&1 ); then
            PROFILE_LANG_VERSION="$(rustc --version | strip)" "$DIR/rust/sort/target/release/profile-sort" \
                || skip_lang rust rust "rust sort runner crashed"
        else
            skip_lang rust rust "cargo build failed (see /tmp/profile-sort-rust.log)"
        fi
    else
        skip_lang rust rust "cargo not installed"
    fi
fi

# ---- cpp: std::sort + vendored pdqsort ----
if want cpp; then
    if command -v g++ >/dev/null 2>&1 && [[ -f "$VENDOR/pdqsort/pdqsort.h" ]]; then
        BIN="$BUILD/cpp-sort"
        if [[ ! -x "$BIN" || "$DIR/cpp/sort/sort_bench.cpp" -nt "$BIN" ]]; then
            g++ -O3 -march=native -DNDEBUG -I "$VENDOR/pdqsort" \
                "$DIR/cpp/sort/sort_bench.cpp" -o "$BIN" >/tmp/profile-sort-cpp.log 2>&1 || true
        fi
        if [[ -x "$BIN" ]]; then
            PROFILE_LANG_VERSION="$(g++ --version | head -1 | strip)" "$BIN" \
                || skip_lang cpp cpp "cpp sort runner crashed"
        else
            skip_lang cpp cpp "g++ build failed (see /tmp/profile-sort-cpp.log)"
        fi
    else
        skip_lang cpp cpp "g++ or vendored pdqsort missing (run vendor.sh pdqsort)"
    fi
fi

# ---- go ----
if want go; then
    if command -v go >/dev/null 2>&1; then
        ( cd "$DIR/go/sort" && PROFILE_LANG_VERSION="$(go version | strip)" go run . 2>/tmp/profile-sort-go.log ) \
            || skip_lang go go "go build/run failed (see /tmp/profile-sort-go.log)"
    else
        skip_lang go go "go not installed"
    fi
fi

# ---- python ----
if want python; then
    PY="$DIR/python/.venv/bin/python"
    [[ -x "$PY" ]] || PY="$(command -v python3 || true)"
    if [[ -n "$PY" ]]; then
        PROFILE_LANG_VERSION="$("$PY" --version 2>&1 | strip)" "$PY" "$DIR/python/sort/sort_bench.py" \
            || skip_lang python python "python sort runner crashed"
    else
        skip_lang python python "python3 not installed"
    fi
fi

# ---- java: java.util.Arrays.sort (dual-pivot quicksort + Timsort, JDK stdlib) ----
if want java; then
    if command -v javac >/dev/null 2>&1; then
        OUT="$BUILD/java-sort"; SRC="$DIR/java/sort/SortBench.java"
        if [[ ! -f "$OUT/SortBench.class" || "$SRC" -nt "$OUT/SortBench.class" ]]; then
            mkdir -p "$OUT"; javac -d "$OUT" "$SRC" >/tmp/profile-sort-java.log 2>&1 || true
        fi
        if [[ -f "$OUT/SortBench.class" ]]; then
            PROFILE_LANG_VERSION="$(java -version 2>&1 | head -1 | strip)" java -cp "$OUT" SortBench \
                || skip_lang java java "java sort runner crashed"
        else
            skip_lang java java "javac build failed (see /tmp/profile-sort-java.log)"
        fi
    else
        skip_lang java java "javac not installed"
    fi
fi
