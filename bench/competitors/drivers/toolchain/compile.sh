#!/usr/bin/env bash
# toolchain/ compile competitors (bench/toolchain/Compile.md): compile-tour-cold,
# compile-tour-warm, compile-10k, compile-lto.
#
# Every row compiles the compile-10k program (datasets/gen_synth.py: one
# description, 200 classes, rendered in each language) and checks that the
# binary it produced prints the line the generator computed (EXPECTED).
#   * compile-10k: a cold build of that program.
#   * compile-tour-cold / -warm: the tour has no equivalent in other languages,
#     so these rows compile the same synthetic program, the nearest in size
#     (the tour is 9.8k lines in 94 files; the program 10.0k lines in 202
#     Cajeta files). Cold = an empty build cache; warm = a no-edit rebuild.
#   * compile-lto: release builds at off / thin / full LTO: cargo
#     (profile.release.lto = "off" / "thin" / "fat") and the fork's clang++
#     (-O2, none / -flto=thin / -flto=full, linked with its lld).
# Builds are the toolchains' defaults for an edit-compile loop, as `cajeta
# build` (debug flavor) is on the Cajeta side: javac; cargo build (dev
# profile, rustc driven by cargo); go build (Go has no unoptimized default);
# g++ -O0 -g through ninja -j<nproc>. Java's warm row recompiles everything:
# javac keeps no build cache.
set -uo pipefail
source "$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )/../../lib" &> /dev/null && pwd )/toolchain.sh"

G="$(synth_tree)" || { for b in compile-tour-cold compile-tour-warm compile-10k compile-lto; do
    skip_row "$b" cpp g++ "gen_synth.py failed"; done; exit 0; }
EXP="$(cat "$G/EXPECTED")"
LINES="$(cat "$G/LINES")"
DS="synth-10k (${LINES} Cajeta lines; 200 classes)"
W="$W/compile"

cold_notes() {
    case "$1" in
        compile-10k) echo "cold: empty build cache" ;;
        compile-tour-cold) echo "cold: empty build cache; the tour has no equivalent tree: compiles the 10k synthetic program (nearest in size)" ;;
        compile-tour-warm) echo "no-edit rebuild with a warm cache; the tour has no equivalent tree: the 10k synthetic program" ;;
    esac
}

java_rows() {   # slug cold|warm
    local d="$W/$1/java"; fresh_copy "$G/java" "$d"
    command -v javac >/dev/null || { skip_row "$1" java javac "javac not installed"; return; }
    local files=( "$d"/src/synth/*.java )
    local before="rm -rf out" n; n="$(cold_notes "$1")"
    if [[ "$2" == warm ]]; then
        javac -d "$d/out" "${files[@]}" || { skip_row "$1" java javac "first build failed"; return; }
        before=""; n="$n; javac keeps no build cache: a no-edit rebuild recompiles every file"
    fi
    cb --bench "$1" --lang java --lang-version "$V_JAVAC" --library javac --library-version "$V_JAVAC" \
       --flags "javac -d out (default -g:source;lines)" --dataset "$DS" --warmup 1 --trials 5 --cwd "$d" \
       ${before:+--before "$before"} --notes "$n" \
       --verify "java -cp out synth.Main" --verify-expect "$EXP" -- javac -d out "${files[@]}"
}

rust_rows() {
    local d="$W/$1/rust"; fresh_copy "$G/rust" "$d"
    command -v cargo >/dev/null || { skip_row "$1" rust cargo "cargo not installed"; return; }
    local before="rm -rf target" n; n="$(cold_notes "$1"); rustc driven by cargo: one crate of 202 modules"
    if [[ "$2" == warm ]]; then
        ( cd "$d" && cargo build -q ) || { skip_row "$1" rust cargo "first build failed"; return; }
        before=""
    fi
    cb --bench "$1" --lang rust --lang-version "$V_RUSTC" --library cargo --library-version "$V_CARGO" \
       --flags "cargo build (dev profile: opt-level 0; debug)" --dataset "$DS" --warmup 1 --trials 5 --cwd "$d" \
       ${before:+--before "$before"} --notes "$n" \
       --verify "./target/debug/synth10k" --verify-expect "$EXP" -- cargo build -q
}

go_rows() {
    local d="$W/$1/go"; fresh_copy "$G/go" "$d"
    command -v go >/dev/null || { skip_row "$1" go go "go not installed"; return; }
    local gc="$W/$1/gocache" before n; n="$(cold_notes "$1"); GOCACHE private to this row"
    before="rm -rf '$gc' synth10k"
    if [[ "$2" == warm ]]; then
        rm -rf "$gc"; ( cd "$d" && GOCACHE="$gc" GOTOOLCHAIN=local go build -o synth10k . ) \
            || { skip_row "$1" go go "first build failed"; return; }
        before=""
    fi
    cb --bench "$1" --lang go --lang-version "$V_GO" --library go --library-version "$V_GO" \
       --flags "go build (default: optimized; GOAMD64=$GOAMD64)" --dataset "$DS" --warmup 1 --trials 5 --cwd "$d" \
       --env "GOCACHE=$gc" --env GOTOOLCHAIN=local \
       ${before:+--before "$before"} --notes "$n" \
       --verify "./synth10k" --verify-expect "$EXP" -- go build -o synth10k .
}

cpp_rows() {
    local d="$W/$1/cpp"; fresh_copy "$G/cpp" "$d"
    command -v g++ >/dev/null && command -v ninja >/dev/null || { skip_row "$1" cpp g++ "g++ or ninja not installed"; return; }
    local before="rm -rf build" n; n="$(cold_notes "$1"); 201 translation units through ninja -j$NPROC"
    if [[ "$2" == warm ]]; then
        ( cd "$d" && ninja -j"$NPROC" >/dev/null ) || { skip_row "$1" cpp g++ "first build failed"; return; }
        before=""; n="$n; warm = ninja with every object present (no-op)"
    fi
    cb --bench "$1" --lang cpp --lang-version "$V_GXX" --library g++ --library-version "$V_GXX" \
       --flags "g++ -O0 -g; ninja $V_NINJA -j$NPROC" --dataset "$DS" --warmup 1 --trials 5 --cwd "$d" \
       ${before:+--before "$before"} --notes "$n" \
       --verify "./build/synth10k" --verify-expect "$EXP" -- ninja -j"$NPROC"
}

for b in compile-tour-cold compile-10k; do
    java_rows "$b" cold; rust_rows "$b" cold; go_rows "$b" cold; cpp_rows "$b" cold
done
java_rows compile-tour-warm warm; rust_rows compile-tour-warm warm; go_rows compile-tour-warm warm; cpp_rows compile-tour-warm warm

# ---- compile-lto ----------------------------------------------------------------
d="$W/compile-lto/rust"; fresh_copy "$G/rust" "$d"
for v in off thin full; do
    lto="$v"; [[ "$v" == full ]] && lto=fat
    if command -v cargo >/dev/null; then
        cb --bench compile-lto --variant "$v" --lang rust --lang-version "$V_RUSTC" --library cargo --library-version "$V_CARGO" \
           --flags "cargo build --release (opt-level 2) profile.release.lto=\"$lto\"" --dataset "$DS" --warmup 1 --trials 5 --cwd "$d" \
           --before "rm -rf target" --notes "cold release build; rustc driven by cargo" \
           --verify "./target/release/synth10k" --verify-expect "$EXP" \
           -- cargo build -q --release --config "profile.release.lto=\"$lto\""
    else
        skip_row compile-lto rust cargo "cargo not installed" "$v"
    fi
done
d="$W/compile-lto/cpp"; fresh_copy "$G/cpp" "$d"
for v in off thin full; do
    if [[ -x "$LLVM_BIN/clang++" && -x "$LLVM_BIN/ld.lld" ]]; then
        fl="-O2"; [[ "$v" != off ]] && fl="-O2 -flto=$v"
        sed -e "s|^cxx = .*|cxx = $LLVM_BIN/clang++|" -e "s|^cxxflags = .*|cxxflags = $fl|" \
            -e "s|^ldflags =.*|ldflags = -fuse-ld=lld|" -e "s|^builddir = .*|builddir = build-$v|" \
            "$d/build.ninja" > "$d/lto-$v.ninja"
        cb --bench compile-lto --variant "$v" --lang cpp --lang-version "$V_CLANG" --library clang++ --library-version "$V_CLANG" \
           --flags "clang++ $fl -fuse-ld=lld; ninja -j$NPROC" --dataset "$DS" --warmup 1 --trials 5 --cwd "$d" \
           --before "rm -rf build-$v" --notes "cold release build; the cajeta-llvm fork's clang and lld" \
           --verify "./build-$v/synth10k" --verify-expect "$EXP" -- ninja -f "lto-$v.ninja" -j"$NPROC"
    else
        skip_row compile-lto cpp clang++ "the fork clang++/ld.lld not found at $LLVM_BIN" "$v"
    fi
done
exit 0
