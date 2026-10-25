#!/usr/bin/env bash
# reflect/ competitors (benchmark-reference unit 13, spec §6.12; bench/reflect/Class.md): one program
# per language under competitors/<lang>/reflect/reflect/, each running reflect/class-lookup (1e7
# lookups by name over 4 classes) and reflect/field-enumerate (the fields of a 20-field class, 1e6
# times) with the API the page names:
#   java    Class.forName (JVM-cached), getDeclaredFields + Field.getName
#   go      reflect.TypeOf (Go has no by-name lookup), NumField + Field(i).Name
#   python  getattr(module, name), vars(obj)
# Standard libraries only. A missing toolchain or a failed build is a `skipped` row per id.
# CAJETA_BENCH_SMOKE=1 selects the smoke sizes (1e6 lookups, 1e5 enumerations). Rows are v1; run.sh lifts them.
set -uo pipefail

DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )/../.." &> /dev/null && pwd )"   # competitors/
BUILD="${DIR}/.build"; mkdir -p "$BUILD"
LOGD="${TMPDIR:-/tmp}"
LANGS="${PROFILE_LANGS:-java go python}"
IDS="class-lookup field-enumerate"
strip() { tr -d ',"'; }
want() { case " $LANGS " in *" $1 "*) return 0;; *) return 1;; esac; }
skip_lang() {   # skip_lang <lang> <lib> <reason>
    local b
    for b in $IDS; do
        echo "1,${PROFILE_RUN_ID:-local},${PROFILE_RUN_TS:-},${b},reflect,,0,,0,$1,,$2,,,0,0,-1,-1,-1,-1,,,-1,-1,-1,-1,-1,skipped,,$3,"
    done
}

if want java; then
    OUT="$BUILD/java-reflect"; SRC="$DIR/java/reflect/reflect/ReflectBench.java"
    if ! command -v javac >/dev/null 2>&1; then skip_lang java java.lang.reflect "javac not installed"
    else
        if [[ ! -f "$OUT/ReflectBench.class" || "$SRC" -nt "$OUT/ReflectBench.class" ]]; then
            mkdir -p "$OUT"; javac -d "$OUT" "$SRC" >"$LOGD/cajeta-reflect-java-build.log" 2>&1 || rm -f "$OUT/ReflectBench.class"
        fi
        if [[ -f "$OUT/ReflectBench.class" ]]; then
            PROFILE_LANG_VERSION="$(java -version 2>&1 | head -1 | strip)" java -cp "$OUT" ReflectBench 2>"$LOGD/cajeta-reflect-java.log" \
                || skip_lang java java.lang.reflect "java reflect runner crashed (see $LOGD/cajeta-reflect-java.log)"
        else skip_lang java java.lang.reflect "javac build failed (see $LOGD/cajeta-reflect-java-build.log)"; fi
    fi
fi

if want go; then
    GD="$DIR/go/reflect/reflect"; GBIN="$BUILD/go-reflect"
    if ! command -v go >/dev/null 2>&1; then skip_lang go reflect "go not installed"
    elif ( cd "$GD" && GOAMD64="${GOAMD64:-v3}" go build -o "$GBIN" . >"$LOGD/cajeta-reflect-go-build.log" 2>&1 ); then
        PROFILE_LANG_VERSION="$(go version | strip)" "$GBIN" 2>"$LOGD/cajeta-reflect-go.log" \
            || skip_lang go reflect "go reflect runner crashed (see $LOGD/cajeta-reflect-go.log)"
    else skip_lang go reflect "go build failed (see $LOGD/cajeta-reflect-go-build.log)"; fi
fi

if want python; then
    PY="$DIR/python/.venv/bin/python"; [[ -x "$PY" ]] || PY="$(command -v python3 || true)"
    if [[ -n "$PY" ]]; then
        PROFILE_LANG_VERSION="$("$PY" --version 2>&1 | strip)" "$PY" "$DIR/python/reflect/reflect/reflect_bench.py" 2>"$LOGD/cajeta-reflect-python.log" \
            || skip_lang python builtins "python reflect runner crashed (see $LOGD/cajeta-reflect-python.log)"
    else skip_lang python builtins "python3 not installed"; fi
fi
exit 0
