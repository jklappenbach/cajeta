#!/usr/bin/env bash
# toolchain/ binary-size competitors (bench/toolchain/BinarySize.md):
# binary-size-hello and binary-size-tour. The `memory` metric of these rows is
# the artifact's size on disk: peak_rss_kb holds it in KiB (rounded up) and the
# notes give the exact bytes. The time columns are one launch, which is the
# "runs" check (the launch must print the expected line).
#   * hello: competitors/<lang>/toolchain/hello/. C++ g++ -O2 -s (libstdc++
#     and libc dynamic, not counted); Rust rustc -O -C strip=symbols (std
#     static); Go go build -ldflags=-s -w (static); Java the jar (the JVM it
#     needs is not counted); Python the .py source (the interpreter is not
#     counted).
#   * tour: the tour has no equivalent in other languages, so these rows size
#     the compile-10k program (datasets/gen_synth.py), the nearest in size,
#     with the same release flags; Java a jar of its classes, Python a zipapp.
# Variant `exe` on every row: the language's deployable artifact (a jar and a
# zipapp count as that), to line up with the Cajeta exe row.
set -uo pipefail
source "$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )/../../lib" &> /dev/null && pwd )/toolchain.sh"
W="$W/binarysize"; rm -rf "$W"; mkdir -p "$W"
H="hello, world"

size_row() {   # slug lang library version flags expect artifact notes argv...
    local b="$1" l="$2" lib="$3" v="$4" f="$5" e="$6" art="$7" n="$8"; shift 8
    if [[ -f "$art" ]]; then
        cb --bench "$b" --variant exe --lang "$l" --lang-version "$v" --library "$lib" --library-version "$v" \
           --flags "$f" --warmup 1 --trials 3 --expect-stdout "$e" --size-of "$art" --notes "$n" -- "$@"
    else
        skip_row "$b" "$l" "$lib" "build failed or toolchain missing" exe
    fi
}

# ---- hello ----
g++ -O2 -s -o "$W/hello-cpp" "$TC/cpp/toolchain/hello/hello.cpp" 2>/dev/null
command -v rustc >/dev/null && rustc -O -C strip=symbols -o "$W/hello-rust" "$TC/rust/toolchain/hello/hello.rs" 2>/dev/null
command -v go >/dev/null && ( cd "$TC/go/toolchain/hello" && GOTOOLCHAIN=local go build -ldflags="-s -w" -o "$W/hello-go" . ) 2>/dev/null
if command -v javac >/dev/null; then
    mkdir -p "$W/java" && javac -d "$W/java" "$TC/java/toolchain/hello/Hello.java" &&
    ( cd "$W/java" && jar --create --file hello.jar --main-class Hello Hello.class )
fi
cp "$TC/python/toolchain/hello/hello.py" "$W/hello.py"
size_row binary-size-hello cpp g++ "$V_GXX" "g++ -O2 -s" "$H" "$W/hello-cpp" "libstdc++ and libc are shared libraries; not counted" "$W/hello-cpp"
size_row binary-size-hello rust rustc "$V_RUSTC" "rustc -O -C strip=symbols" "$H" "$W/hello-rust" "std linked statically; libc shared" "$W/hello-rust"
size_row binary-size-hello go go "$V_GO" "go build -ldflags='-s -w'" "$H" "$W/hello-go" "static" "$W/hello-go"
size_row binary-size-hello java javac "$V_JAVAC" "javac; jar" "$H" "$W/java/hello.jar" "the jar only; the JVM it runs on is not counted" java -jar "$W/java/hello.jar"
size_row binary-size-hello python cpython "$V_CPY" "the .py source" "$H" "$W/hello.py" "the source only; the interpreter is not counted" "$CPY" "$W/hello.py"

# ---- tour (the compile-10k program) ----
G="$(synth_tree)"; EXP="$(cat "$G/EXPECTED")"
T="the tour has no equivalent: sizes the 10k synthetic program (nearest in size)"
fresh_copy "$G/cpp" "$W/cpp" && sed -e 's|^cxxflags = .*|cxxflags = -O2|' -e 's|^ldflags =.*|ldflags = -s|' "$W/cpp/build.ninja" > "$W/cpp/rel.ninja" &&
    ( cd "$W/cpp" && ninja -f rel.ninja -j"$NPROC" >/dev/null )
size_row binary-size-tour cpp g++ "$V_GXX" "g++ -O2 -s" "$EXP" "$W/cpp/build/synth10k" "$T; libstdc++ shared" "$W/cpp/build/synth10k"
if command -v cargo >/dev/null; then
    fresh_copy "$G/rust" "$W/rust" && ( cd "$W/rust" && cargo build -q --release --config 'profile.release.strip="symbols"' ) 2>/dev/null
fi
size_row binary-size-tour rust cargo "$V_RUSTC" "cargo build --release (opt-level 2) strip=symbols" "$EXP" "$W/rust/target/release/synth10k" "$T" "$W/rust/target/release/synth10k"
if command -v go >/dev/null; then
    fresh_copy "$G/go" "$W/go" && ( cd "$W/go" && GOTOOLCHAIN=local go build -ldflags="-s -w" -o synth10k . ) 2>/dev/null
fi
size_row binary-size-tour go go "$V_GO" "go build -ldflags='-s -w'" "$EXP" "$W/go/synth10k" "$T; static" "$W/go/synth10k"
if command -v javac >/dev/null; then
    fresh_copy "$G/java" "$W/jsynth" && ( cd "$W/jsynth" && javac -d out src/synth/*.java && jar --create --file synth.jar --main-class synth.Main -C out . )
fi
size_row binary-size-tour java javac "$V_JAVAC" "javac; jar" "$EXP" "$W/jsynth/synth.jar" "$T; the JVM is not counted" java -jar "$W/jsynth/synth.jar"
fresh_copy "$G/python" "$W/pysynth" && "$CPY" -m zipapp "$W/pysynth" -m "synth.__main__:main" -o "$W/synth.pyz"
size_row binary-size-tour python cpython "$V_CPY" "python -m zipapp (sources)" "$EXP" "$W/synth.pyz" "$T; the interpreter is not counted" "$CPY" "$W/synth.pyz"
exit 0
