// Windows-only symbol bridge for the in-process JIT host: MinGW links the CRT
// statically, so those functions are absent from the PE export table and the JIT's
// DynamicLibrarySearchGenerator cannot see them — install an absolute table instead.
#pragma once

#include <stddef.h>

namespace cajeta::jit {

    struct JitWinSym {
        const char* name;
        void* addr;
    };

    // Returns the (name, address) table and its length via *count; nullptr with
    // *count = 0 on non-Windows targets.
    const JitWinSym* winJitSymbols(size_t* count);

} // namespace cajeta::jit
