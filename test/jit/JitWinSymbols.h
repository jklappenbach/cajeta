#ifndef CAJETA_TEST_JIT_WIN_SYMBOLS_H
#define CAJETA_TEST_JIT_WIN_SYMBOLS_H

// Windows-only bridge: the embedded runtime bitcode the JIT loads calls a
// handful of MinGW CRT / libgcc functions (write, open, __mingw_fprintf,
// stat64i32, ___chkstk_ms, ...). Those are statically linked into the test
// binary and therefore NOT in its PE export table, so LLJIT's
// DynamicLibrarySearchGenerator (which dlsym's the process) can't find them.
// JitWinSymbols.c takes their addresses in a TU compiled with the same MinGW
// headers/flags as the runtime, and JitTestHelper binds them into the JIT
// dylib via absoluteSymbols(). See JitWinSymbols.c for the full rationale.

#ifdef _WIN32

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CajetaJitWinSym {
    const char* name;  // symbol name exactly as the JIT requests it
    void* addr;        // real address in the linked test binary
} CajetaJitWinSym;

// Returns the {name, addr} table; *count receives the entry count.
const CajetaJitWinSym* cajeta_jit_win_symbols(size_t* count);

#ifdef __cplusplus
}
#endif

#endif  // _WIN32
#endif  // CAJETA_TEST_JIT_WIN_SYMBOLS_H
