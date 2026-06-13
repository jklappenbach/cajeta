//
// fp128 (`long double` / `__float128`) compiler-rt builtin bridge for the
// in-process JIT host. Sibling to CajetaJitWinSymbols.* — same mechanism
// (a name->addr table installed via absoluteSymbols), different motivation.
//
// The cajeta stdlib uses fp128 in a few numeric paths, so JIT'd code emits
// the soft-float helper calls compiler-rt provides (__trunctfdf2,
// __fixtfdi, __addtf3, ...). On x86_64-linux those resolve through the JIT's
// DynamicLibrarySearchGenerator because libgcc is linked into the host
// process AND dynamically exported (verified: `gcc -print-libgcc-file-name`
// ships trunctfdf2.o / fixtfdi.o). On macOS the equivalent helpers live in
// the statically-linked compiler-rt builtins archive and are NOT in the
// host's dynamic export table, so the generator can't see them and every
// JIT'd program that touches fp128 fails to materialize with
//   JIT session error: Symbols not found: [ __trunctfdf2, __fixtfdi ]
// which on the macOS arm64 release leg failed all 424 release tests (the
// link error fires before any test body runs).
//
// Fix: take each helper's address in this TU (forcing the linker to pull it
// from compiler-rt) and hand the host a name->addr table to install via
// absoluteSymbols(). APPLE-only: on Linux/Windows the table is empty so the
// TU references nothing and imposes no link requirement.
//
#pragma once

#include <stddef.h>

namespace cajeta::jit {

    struct JitFloatSym {
        const char* name;
        void* addr;
    };

    // Returns the (name, address) table and its length via *count. Empty
    // (returns nullptr, *count = 0) on non-Apple targets, where the fp128
    // helpers resolve from the dynamically-exported host libgcc instead.
    const JitFloatSym* floatJitSymbols(size_t* count);

} // namespace cajeta::jit
