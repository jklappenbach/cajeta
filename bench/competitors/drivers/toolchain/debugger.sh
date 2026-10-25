#!/usr/bin/env bash
# toolchain/debugger-attach competitors (bench/toolchain/Debugger.md): attach a
# debugger to a running process, set one breakpoint, wait for it to be
# reported, detach; 20 times against one debuggee (competitors/<lang>/
# toolchain/tick/, a loop calling tick() every millisecond, started once and
# outside the timed region). Each trial is one attach_once.py process.
#   jdb   -> a JVM started with a JDWP socket agent (suspend=n)
#   gdb   -> the Rust (rustc -g) and the C++ (g++ -O1 -g) tick binaries
#   delve -> the Go tick binary (-gcflags=all=-N -l); dlv 1.25.2, pinned
#   debugpy -> CPython under `python -m debugpy --listen`, a DAP client attaches
# The native debuggees call prctl(PR_SET_PTRACER, ANY) so a non-parent
# debugger may attach under Yama ptrace_scope=1 without sudo.
set -uo pipefail
source "$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )/../../lib" &> /dev/null && pwd )/toolchain.sh"
W="$W/debugger"; mkdir -p "$W"
A="$TC/python/toolchain/attach_once.py"
N="attach to a running process; one breakpoint; detach"
PIDS=()
cleanup() { for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; wait "$p" 2>/dev/null; done; }
trap cleanup EXIT

# start <argv...>: run a debuggee in the background, wait for its "ready" line.
start() {
    local log="$W/debuggee-$RANDOM.log"
    "$@" >"$log" 2>&1 &
    local pid=$!
    local i=0
    while (( i < 200 )); do grep -q ready "$log" 2>/dev/null && break; sleep 0.05; i=$((i+1)); done
    PIDS+=("$pid"); STARTED="$pid"
    grep -q ready "$log"
}
free_port() { "$CPY" -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])'; }

# ---- jdb ----
if command -v jdb >/dev/null && javac -d "$W/java" "$TC/java/toolchain/tick/Tick.java" 2>/dev/null; then
    P="$(free_port)"
    if start java -agentlib:jdwp=transport=dt_socket,server=y,suspend=n,address=127.0.0.1:"$P" -cp "$W/java" Tick; then
        cb --bench debugger-attach --lang java --lang-version "$V_JAVA" --library jdb --library-version "$V_JAVA" \
           --flags "jdb -attach (JDWP dt_socket)" --warmup 1 --trials 20 --expect-stdout "breakpoint reported" \
           --notes "$N" -- "$CPY" "$A" jdb "$P" Tick.tick
    else skip_row debugger-attach java jdb "the JDWP debuggee did not start"; fi
else skip_row debugger-attach java jdb "jdb/javac not installed"; fi

# ---- gdb: Rust and C++ ----
GV="$(gdb --version 2>/dev/null | strip_c)"
if command -v gdb >/dev/null; then
    if command -v rustc >/dev/null && rustc -g -C opt-level=1 -o "$W/tick-rust" "$TC/rust/toolchain/tick/tick.rs" 2>/dev/null \
            && start "$W/tick-rust"; then
        cb --bench debugger-attach --lang rust --lang-version "$V_RUSTC" --library gdb --library-version "$GV" \
           --flags "gdb -batch -p <pid>; rustc -g -C opt-level=1" --warmup 1 --trials 20 --expect-stdout "breakpoint reported" \
           --notes "$N" -- "$CPY" "$A" gdb "$STARTED" tick
    else skip_row debugger-attach rust gdb "rustc missing or the debuggee did not start"; fi
    if g++ -O1 -g -o "$W/tick-cpp" "$TC/cpp/toolchain/tick/tick.cpp" 2>/dev/null && start "$W/tick-cpp"; then
        cb --bench debugger-attach --lang cpp --lang-version "$V_GXX" --library gdb --library-version "$GV" \
           --flags "gdb -batch -p <pid>; g++ -O1 -g" --warmup 1 --trials 20 --expect-stdout "breakpoint reported" \
           --notes "$N" -- "$CPY" "$A" gdb "$STARTED" tick
    else skip_row debugger-attach cpp gdb "the debuggee did not build or start"; fi
else
    skip_row debugger-attach rust gdb "gdb not installed"; skip_row debugger-attach cpp gdb "gdb not installed"
fi

# ---- delve ----
DLV="$TC/bin/dlv"
if [[ -x "$DLV" ]] && ( cd "$TC/go/toolchain/tick" && GOTOOLCHAIN=local go build -gcflags='all=-N -l' -o "$W/tick-go" . ) 2>/dev/null \
        && start "$W/tick-go"; then
    DV="$("$DLV" version 2>/dev/null | sed -n 's/^Version: //p' | strip_c)"
    cb --bench debugger-attach --lang go --lang-version "$V_GO" --library delve --library-version "$DV" \
       --flags "dlv attach <pid>; go build -gcflags=all=-N -l" --warmup 1 --trials 20 --expect-stdout "breakpoint reported" \
       --notes "$N" -- "$CPY" "$A" dlv "$DLV" "$STARTED" main.tick
else skip_row debugger-attach go delve "dlv not provisioned (setup.sh: competitors/bin/dlv) or the debuggee did not start"; fi

# ---- debugpy ----
TICK="$TC/python/toolchain/tick/tick.py"
LINE="$(grep -n "BREAK HERE" "$TICK" | cut -d: -f1)"
if "$PY" -c "import debugpy" 2>/dev/null; then
    P="$(free_port)"
    if start "$PY" -m debugpy --listen 127.0.0.1:"$P" "$TICK"; then
        DBV="$("$PY" -c 'import importlib.metadata as m; print(m.version("debugpy"))')"
        cb --bench debugger-attach --lang python --lang-version "$V_CPY" --library debugpy --library-version "$DBV" \
           --flags "python -m debugpy --listen; DAP attach" --warmup 1 --trials 20 --expect-stdout "breakpoint reported" \
           --notes "$N" -- "$CPY" "$A" debugpy "$P" "$TICK" "$LINE"
    else skip_row debugger-attach python debugpy "the debugpy debuggee did not start"; fi
else skip_row debugger-attach python debugpy "debugpy not in the venv (setup.sh)"; fi
exit 0
