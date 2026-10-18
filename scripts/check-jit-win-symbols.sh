#!/usr/bin/env bash
# Every runtime import the Windows JIT finds only through its symbol bridge
# (src/cajeta/jit/CajetaJitWinSymbols.cpp) must be bound there. Runs on Linux,
# so the gap is caught before a Windows build is attempted. The
# `__cajeta_tls_*` half of this check left with the TLS engine
# (cajeta-crypto spec 6.3); dev.cajeta.crypto's shim carries its own.
set -u
cd "$(dirname "$0")/.."
missing=0
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
