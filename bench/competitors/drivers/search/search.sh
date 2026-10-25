#!/usr/bin/env bash
# search/ competitors (benchmark-reference unit 13, bench/search/*.md): one
# program per language under competitors/<lang>/search/search/, each running
# the search/ ids its page rows name for that language, on the inputs of the
# shared description (bench/scripts/search_ref.py: LCG31, seeds 12345, 777,
# 999) and checked against search_ref.py's constants:
#   rust    strsim (distance), nucleo-matcher (fuzzy-topk), std HashMap (ngram)
#   python  rapidfuzz (distance, process.extract for fuzzy-topk), dict (ngram)
#   java    commons-text (distance), Lucene FuzzyQuery (fuzzy-topk), Lucene NGramTokenizer index (ngram)
#   go      agnivade/levenshtein, xrash/smetrics Jaro-Winkler (distance only, as the page names)
# Rows are v1 (31 columns, area search); run.sh turns them into schema v2.
# A missing toolchain or library is a `skipped` row per id with the reason.
# CAJETA_BENCH_SMOKE=1 selects the smoke sizes (every row says which in notes).
# Env: PROFILE_RUN_ID, PROFILE_RUN_TS, PROFILE_WARMUP, PROFILE_TRIALS, PROFILE_LANGS.
set -uo pipefail

DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )/../.." &> /dev/null && pwd )"   # competitors/
BUILD="${DIR}/.build"; mkdir -p "$BUILD"
LOGS="${DIR}/logs"; mkdir -p "$LOGS"
TP="$DIR/third_party"
LANGS="${PROFILE_LANGS:-rust python java go}"
[[ -d "$HOME/.cargo/bin" ]] && export PATH="$HOME/.cargo/bin:$PATH"
strip() { tr -d ',"'; }
want() { case " $LANGS " in *" $1 "*) return 0;; *) return 1;; esac; }

ALL="distance-levenshtein distance-jaro-winkler fuzzy-topk ngram-build ngram-query"
DIST="distance-levenshtein distance-jaro-winkler"
skip_ids() {   # lang lib reason ids...
    local lang="$1" lib="$2" reason="$3" rid="${PROFILE_RUN_ID:-local}" ts="${PROFILE_RUN_TS:-}" b; shift 3
    for b in "$@"; do
        echo "1,${rid},${ts},${b},search,,0,,0,${lang},,${lib},,,0,0,-1,-1,-1,-1,,,-1,-1,-1,-1,-1,skipped,,${reason},"
    done
}

if want rust; then
    RD="$DIR/rust/search/search"
    if command -v cargo >/dev/null 2>&1; then
        if ( cd "$RD" && RUSTFLAGS="-C target-cpu=native" cargo build --release --locked >"$LOGS/search-rust-build.log" 2>&1 ); then
            PROFILE_LANG_VERSION="$(rustc --version | strip)" "$RD/target/release/search-search" 2>"$LOGS/search-rust.log" \
                || skip_ids rust strsim "rust search runner failed (logs/search-rust.log)" $ALL
        else skip_ids rust strsim "cargo build failed (logs/search-rust-build.log)" $ALL; fi
    else skip_ids rust strsim "cargo not installed (run competitors/setup.sh)" $ALL; fi
fi

if want python; then
    PY="$DIR/python/.venv/bin/python"
    if [[ -x "$PY" ]] && "$PY" -c 'import rapidfuzz' 2>/dev/null; then
        PROFILE_LANG_VERSION="$("$PY" --version 2>&1 | strip)" "$PY" "$DIR/python/search/search/search_bench.py" 2>"$LOGS/search-python.log" \
            || skip_ids python rapidfuzz "python search runner crashed (logs/search-python.log)" $ALL
    else skip_ids python rapidfuzz "venv with rapidfuzz missing (run competitors/setup.sh)" $ALL; fi
fi

if want java; then
    JBIN="$HOME/.sdkman/candidates/java/current/bin"; [[ -x "$JBIN/javac" ]] || JBIN="$(dirname "$(command -v javac 2>/dev/null || echo /nonexistent/javac)")"
    J="$TP/java"
    JCP="$J/org/apache/commons/commons-text/1.15.0/commons-text-1.15.0.jar:$J/org/apache/commons/commons-lang3/3.20.0/commons-lang3-3.20.0.jar"
    JCP="$JCP:$J/org/apache/lucene/lucene-core/10.5.2/lucene-core-10.5.2.jar:$J/org/apache/lucene/lucene-analysis-common/10.5.2/lucene-analysis-common-10.5.2.jar"
    missing=""
    for f in ${JCP//:/ }; do [[ -f "$f" ]] || missing="$missing $(basename "$f")"; done
    if [[ ! -x "$JBIN/javac" ]]; then
        skip_ids java commons-text "javac not installed" $ALL
    elif [[ -n "$missing" ]]; then
        skip_ids java commons-text "pinned jars missing:$missing (run competitors/setup.sh)" $ALL
    else
        OUT="$BUILD/java-search"; SRC="$DIR/java/search/search/SearchBench.java"
        if [[ ! -f "$OUT/SearchBench.class" || "$SRC" -nt "$OUT/SearchBench.class" ]]; then
            mkdir -p "$OUT"; "$JBIN/javac" -cp "$JCP" -d "$OUT" "$SRC" >"$LOGS/search-java-build.log" 2>&1 || rm -f "$OUT/SearchBench.class"
        fi
        if [[ -f "$OUT/SearchBench.class" ]]; then
            # The Vector API module is what Lucene asks for to use its SIMD paths.
            PROFILE_LANG_VERSION="$("$JBIN/java" -version 2>&1 | head -1 | strip)" PROFILE_JAVA_FLAGS="--add-modules jdk.incubator.vector" \
                "$JBIN/java" --add-modules jdk.incubator.vector -Xmx8g -cp "$OUT:$JCP" SearchBench 2>"$LOGS/search-java.log" \
                || skip_ids java commons-text "java search runner crashed (logs/search-java.log)" $ALL
        else skip_ids java commons-text "javac build failed (logs/search-java-build.log)" $ALL; fi
    fi
fi

if want go; then
    if command -v go >/dev/null 2>&1; then
        GBIN="$BUILD/go-search"
        if ( cd "$DIR/go/search/search" && GOAMD64="${GOAMD64:-v3}" go build -mod=mod -o "$GBIN" . >"$LOGS/search-go-build.log" 2>&1 ); then
            PROFILE_LANG_VERSION="$(go version | strip)" "$GBIN" 2>"$LOGS/search-go.log" \
                || skip_ids go agnivade/levenshtein "go search runner failed (logs/search-go.log)" $DIST
        else skip_ids go agnivade/levenshtein "go build failed (logs/search-go-build.log)" $DIST; fi
    else skip_ids go agnivade/levenshtein "go not installed" $DIST; fi
fi
exit 0
