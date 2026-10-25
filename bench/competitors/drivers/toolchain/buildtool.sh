#!/usr/bin/env bash
# toolchain/build-noop competitors (bench/toolchain/BuildTool.md): a no-op
# build of a hello-world project, 1e2 times, after one real build. Each trial
# is one build-tool invocation that finds nothing to do; the check is exit 0.
#   * Gradle 9.8.2 (third_party/gradle/, pinned in pins.tsv; the sdkman Gradle 8.10
#     cannot run on Java 25): `gradle --offline -q build`, with its daemon,
#     the default (the warm-up starts it; the driver stops it afterwards).
#   * Maven: `mvn -o -q package`; the first build runs online once to fill
#     ~/.m2 with the plugin versions the pom pins.
#   * cargo build, go build, ninja: their no-edit rebuilds.
set -uo pipefail
source "$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )/../../lib" &> /dev/null && pwd )/toolchain.sh"
W="$W/buildtool"; mkdir -p "$W"
N="no-op: one invocation after a first build; no edits"

GRADLE="$TC/third_party/gradle/gradle-9.8.2/bin/gradle"
fresh_copy "$TC/java/toolchain/buildnoop" "$W/gradle"
if [[ -x "$GRADLE" ]] && ( cd "$W/gradle" && "$GRADLE" --offline -q build ) >"$W/gradle.log" 2>&1; then
    GV="$("$GRADLE" --version 2>/dev/null | sed -n 's/^Gradle //p' | strip_c)"
    cb --bench build-noop --lang java --lang-version "$V_JAVA" --library gradle --library-version "$GV" \
       --flags "gradle --offline -q build (daemon)" --warmup 3 --trials 100 --cwd "$W/gradle" \
       --notes "$N; the Gradle daemon is warm (started by the first build)" -- "$GRADLE" --offline -q build
    ( cd "$W/gradle" && "$GRADLE" --stop >/dev/null 2>&1 )
else
    skip_row build-noop java gradle "Gradle 9.8.2 not provisioned (setup.sh) or its first build failed (see $W/gradle.log)"
fi

fresh_copy "$TC/java/toolchain/buildnoop" "$W/maven"
if command -v mvn >/dev/null && ( cd "$W/maven" && mvn -q package ) >"$W/maven.log" 2>&1; then
    MV="$(mvn -v 2>/dev/null | head -1 | sed -E 's/\x1b\[[0-9;]*m//g' | strip_c)"
    cb --bench build-noop --lang java --lang-version "$V_JAVA" --library maven --library-version "$MV" \
       --flags "mvn -o -q package" --warmup 3 --trials 100 --cwd "$W/maven" --notes "$N" -- mvn -o -q package
else
    skip_row build-noop java maven "mvn missing or its first build failed (see $W/maven.log)"
fi

fresh_copy "$TC/rust/toolchain/buildnoop" "$W/cargo"
if command -v cargo >/dev/null && ( cd "$W/cargo" && cargo build -q ) >"$W/cargo.log" 2>&1; then
    cb --bench build-noop --lang rust --lang-version "$V_RUSTC" --library cargo --library-version "$V_CARGO" \
       --flags "cargo build" --warmup 3 --trials 100 --cwd "$W/cargo" --notes "$N" -- cargo build -q
else
    skip_row build-noop rust cargo "cargo missing or its first build failed"
fi

fresh_copy "$TC/go/toolchain/buildnoop" "$W/go"
if command -v go >/dev/null && ( cd "$W/go" && GOTOOLCHAIN=local go build -o hello . ) >"$W/go.log" 2>&1; then
    cb --bench build-noop --lang go --lang-version "$V_GO" --library go --library-version "$V_GO" \
       --flags "go build -o hello ." --warmup 3 --trials 100 --cwd "$W/go" --env GOTOOLCHAIN=local \
       --notes "$N; the user build cache is warm" -- go build -o hello .
else
    skip_row build-noop go go "go missing or its first build failed"
fi

fresh_copy "$TC/cpp/toolchain/buildnoop" "$W/ninja"
if command -v ninja >/dev/null && ( cd "$W/ninja" && ninja ) >"$W/ninja.log" 2>&1; then
    cb --bench build-noop --lang cpp --lang-version "$V_GXX" --library ninja --library-version "$V_NINJA" \
       --flags "ninja (g++ -O2)" --warmup 3 --trials 100 --cwd "$W/ninja" --notes "$N" -- ninja
else
    skip_row build-noop cpp ninja "ninja missing or its first build failed"
fi
exit 0
