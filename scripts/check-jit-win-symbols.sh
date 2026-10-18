#!/usr/bin/env bash
# Every `__cajeta_tls_*` entry point the runtime defines in
# runtime/native/cajeta_tls.c must be bound in the Windows JIT symbol bridge
# (src/cajeta/jit/CajetaJitWinSymbols.cpp) AND in the test harness's own bind
# list (test/jit/JitTestHelper.cpp). cajeta_tls.c is linked natively,
# not embedded in the runtime bitcode, so on COFF the JIT finds its symbols only
# through that table: a definition missing from it fails EVERY JIT test on
# Windows with "Symbols not found: [ __cajeta_tls_... ]", which is how the first
# v0.43.0 cut lost its Windows leg (143 of 902) over two unbound entries.
# Runs on Linux, so the gap is caught before a Windows build is attempted.
set -u
cd "$(dirname "$0")/.."
missing=0
while read -r name; do
    [ -z "$name" ] && continue
    if ! grep -q "CJ_SYM(\"$name\"" src/cajeta/jit/CajetaJitWinSymbols.cpp; then
        echo "check-jit-win-symbols: $name is defined in runtime/native/cajeta_tls.c but not bound in src/cajeta/jit/CajetaJitWinSymbols.cpp" >&2
        missing=$((missing + 1))
    fi
    # The test harness binds its own list for the JITs it builds itself.
    if ! grep -q "bind(\"$name\"" test/jit/JitTestHelper.cpp; then
        echo "check-jit-win-symbols: $name is defined in runtime/native/cajeta_tls.c but not bound in test/jit/JitTestHelper.cpp" >&2
        missing=$((missing + 1))
    fi
done < <(grep -oE '^(int|int64_t|void|void\*|const char\*|long)[a-zA-Z0-9_ *]* __cajeta_tls_[a-zA-Z0-9_]+' runtime/native/cajeta_tls.c | awk '{print $NF}' | sort -u)
# The runtime bitcode's imports from winpthread, the mingw 64-bit time calls
# and libgcc's emutls. The release cajeta.exe is linked -static, so nothing in
# it is on the PE export table and its JIT finds these only through the bridge.
# The test binary links winpthread as a DLL, so the suite never saw the gap:
# measured 2026-10-17 on the published v0.43.1 zip, `cajeta jit-run` failed
# for every program, a stdlib-only one included, naming exactly this list.
for name in __emutls_get_address clock_gettime64 nanosleep64 \
        pthread_cond_broadcast pthread_cond_destroy pthread_cond_init \
        pthread_cond_signal pthread_cond_timedwait64 pthread_cond_wait \
        pthread_create pthread_equal pthread_join pthread_mutex_destroy \
        pthread_mutex_init pthread_mutex_lock pthread_mutex_unlock \
        pthread_mutexattr_destroy pthread_mutexattr_init \
        pthread_mutexattr_settype pthread_once pthread_self; do
    if ! grep -q "CJ_SYM(\"$name\"" src/cajeta/jit/CajetaJitWinSymbols.cpp; then
        echo "check-jit-win-symbols: $name is imported by the runtime bitcode on Windows but not bound in src/cajeta/jit/CajetaJitWinSymbols.cpp" >&2
        missing=$((missing + 1))
    fi
done
if [ "$missing" -gt 0 ]; then
    echo "check-jit-win-symbols: $missing unbound" >&2
    exit 1
fi
echo "check-jit-win-symbols: OK"
