//
// Windows-only symbol bridge for the in-process JIT host. Mirrors
// test/jit/JitWinSymbols.* but lives in src so the `cajeta` binary's JIT host
// can install it.
//
// On Linux/macOS the JIT's DynamicLibrarySearchGenerator resolves libc
// functions (write, open, fprintf, ...) because they're dynamically exported.
// On Windows/MinGW those CRT functions are statically linked into the host
// binary and absent from its PE export table, so the generator can't see them
// and every runtime-dependent JIT module fails to materialize with
//   Symbols not found: [ write, open, __mingw_fprintf, stat64i32, ... ]
//
// Fix: take each function's address in a TU compiled with the same MinGW
// headers as runtime/native/cajeta_runtime.c and hand the host a name->addr
// table to install via absoluteSymbols().
//
#pragma once

#include <stddef.h>

namespace cajeta::jit {

    struct JitWinSym {
        const char* name;
        void* addr;
    };

    // Returns the (name, address) table and its length via *count. Empty
    // (returns nullptr, *count = 0) on non-Windows targets.
    const JitWinSym* winJitSymbols(size_t* count);

} // namespace cajeta::jit
