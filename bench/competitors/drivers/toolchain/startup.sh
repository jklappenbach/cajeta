#!/usr/bin/env bash
# toolchain/ start-up competitors (bench/toolchain/Startup.md): startup-exe,
# startup-cja-cold, startup-cja-warm. Each trial is one launch of hello-world
# (competitors/<lang>/toolchain/hello/), checked for its "hello, world" line.
#   * startup-exe: C++ (g++ -O2), Rust (rustc -O), Go (go build) binaries,
#     Java `java -jar hello.jar` (the default CDS archive on), CPython running
#     hello.py; 1e2 launches.
#   * startup-cja-cold: Java with class-data sharing off (-Xshare:off) and
#     CPython importing hello with an empty bytecode cache
#     (PYTHONPYCACHEPREFIX emptied before every launch, so every stdlib module
#     it imports is compiled from source too).
#   * startup-cja-warm: Java with an AppCDS archive of the program
#     (-XX:SharedArchiveFile, made once with -XX:ArchiveClassesAtExit) and
#     CPython with that cache populated.
# Both sides time fork/exec/wait from their own harness (Python's subprocess
# here, the cajeta runtime's Command there).
set -uo pipefail
source "$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )/../../lib" &> /dev/null && pwd )/toolchain.sh"
W="$W/startup"; mkdir -p "$W"
H="hello, world"

# ---- builds (untimed) ----
g++ -O2 -o "$W/hello-cpp" "$TC/cpp/toolchain/hello/hello.cpp" 2>"$W/cpp.log" || rm -f "$W/hello-cpp"
command -v rustc >/dev/null && { rustc -O -o "$W/hello-rust" "$TC/rust/toolchain/hello/hello.rs" 2>"$W/rust.log" || rm -f "$W/hello-rust"; }
command -v go >/dev/null && { ( cd "$TC/go/toolchain/hello" && GOTOOLCHAIN=local go build -o "$W/hello-go" . ) 2>"$W/go.log" || rm -f "$W/hello-go"; }
JOK=0
if command -v javac >/dev/null; then
    rm -rf "$W/java" && mkdir -p "$W/java" &&
    javac -d "$W/java" "$TC/java/toolchain/hello/Hello.java" &&
    ( cd "$W/java" && jar --create --file hello.jar --main-class Hello Hello.class ) &&
    rm -f "$W/java/hello.jsa" &&
    java -XX:ArchiveClassesAtExit="$W/java/hello.jsa" -jar "$W/java/hello.jar" >/dev/null 2>"$W/java-cds.log" &&
    JOK=1
fi
mkdir -p "$W/py" && cp "$TC/python/toolchain/hello/hello.py" "$W/py/hello.py"

native() {   # lang library version bin flags
    if [[ -x "$4" ]]; then
        cb --bench startup-exe --lang "$1" --lang-version "$3" --library "$2" --library-version "$3" \
           --flags "$5" --warmup 3 --trials 100 --expect-stdout "$H" --notes "one launch per trial" -- "$4"
    else
        skip_row startup-exe "$1" "$2" "build failed or toolchain missing"
    fi
}
native cpp g++ "$V_GXX" "$W/hello-cpp" "g++ -O2"
native rust rustc "$V_RUSTC" "$W/hello-rust" "rustc -O"
native go go "$V_GO" "$W/hello-go" "go build"

if (( JOK )); then
    cb --bench startup-exe --lang java --lang-version "$V_JAVA" --library java --library-version "$V_JAVA" \
       --flags "java -jar (default CDS archive)" --warmup 3 --trials 100 --expect-stdout "$H" \
       --notes "one launch per trial" -- java -jar "$W/java/hello.jar"
    cb --bench startup-cja-cold --lang java --lang-version "$V_JAVA" --library java --library-version "$V_JAVA" \
       --flags "java -Xshare:off -jar" --warmup 1 --trials 20 --expect-stdout "$H" \
       --notes "cold: class-data sharing off" -- java -Xshare:off -jar "$W/java/hello.jar"
    cb --bench startup-cja-warm --lang java --lang-version "$V_JAVA" --library java-cds --library-version "$V_JAVA" \
       --flags "java -XX:SharedArchiveFile=hello.jsa -jar" --warmup 3 --trials 20 --expect-stdout "$H" \
       --notes "warm: AppCDS archive of the program" -- java -XX:SharedArchiveFile="$W/java/hello.jsa" -jar "$W/java/hello.jar"
else
    for b in startup-exe startup-cja-cold; do skip_row "$b" java java "javac/jar or the CDS dump failed"; done
    skip_row startup-cja-warm java java-cds "javac/jar or the CDS dump failed"
fi

cb --bench startup-exe --lang python --lang-version "$V_CPY" --library cpython --library-version "$V_CPY" \
   --flags "python3 hello.py" --warmup 3 --trials 100 --expect-stdout "$H" \
   --notes "one launch per trial; CPython as shipped" -- "$CPY" "$W/py/hello.py"
PC="$W/py/pycache"
cb --bench startup-cja-cold --lang python --lang-version "$V_CPY" --library cpython --library-version "$V_CPY" \
   --flags "python3 -c 'import hello' with an empty PYTHONPYCACHEPREFIX" --warmup 1 --trials 20 --expect-stdout "$H" \
   --env "PYTHONPATH=$W/py" --env "PYTHONPYCACHEPREFIX=$PC" --before "rm -rf '$PC'" \
   --notes "cold: no cached bytecode for the program or the stdlib it imports" -- "$CPY" -c "import hello"
rm -rf "$PC"; PYTHONPATH="$W/py" PYTHONPYCACHEPREFIX="$PC" "$CPY" -c "import hello" >/dev/null
cb --bench startup-cja-warm --lang python --lang-version "$V_CPY" --library cpython --library-version "$V_CPY" \
   --flags "python3 -c 'import hello' with a populated PYTHONPYCACHEPREFIX" --warmup 3 --trials 20 --expect-stdout "$H" \
   --env "PYTHONPATH=$W/py" --env "PYTHONPYCACHEPREFIX=$PC" \
   --notes "warm: .pyc cached for the program and the stdlib" -- "$CPY" -c "import hello"
exit 0
