#!/usr/bin/env bash
# Build + run one language's "hello-metric" runner, which emits a single
# schema-conformant CSV row (the cross-language CSV contract every area unit
# builds on). Detects the toolchain version and passes it in via
# PROFILE_LANG_VERSION. Usage: hello.sh <rust|cpp|java|python|go>
set -euo pipefail

DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
lang="${1:?usage: hello.sh <lang>}"
TMP="${TMPDIR:-/tmp}/profile-hello"
mkdir -p "$TMP"
strip() { tr -d ',"'; }

case "$lang" in
  rust)
    rustc -O -o "$TMP/hello-rust" "$DIR/rust/hello.rs" >/dev/null 2>&1
    PROFILE_LANG_VERSION="$(rustc --version | strip)" "$TMP/hello-rust" ;;
  cpp)
    g++ -O2 -o "$TMP/hello-cpp" "$DIR/cpp/hello.cpp"
    PROFILE_LANG_VERSION="$(g++ --version | head -1 | strip)" "$TMP/hello-cpp" ;;
  java)
    javac -d "$TMP" "$DIR/java/Hello.java"
    PROFILE_LANG_VERSION="$(java -version 2>&1 | head -1 | strip)" java -cp "$TMP" Hello ;;
  python)
    PROFILE_LANG_VERSION="$(python3 --version | strip)" python3 "$DIR/python/hello.py" ;;
  go)
    ( cd "$DIR/go" && PROFILE_LANG_VERSION="$(go version | strip)" go run hello.go ) ;;
  *)
    echo "unknown lang: $lang" >&2; exit 2 ;;
esac
