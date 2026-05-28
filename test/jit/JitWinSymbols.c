// Windows-only symbol bridge for the JIT. See JitWinSymbols.h for the why.
//
// On Linux/macOS the JIT's DynamicLibrarySearchGenerator resolves libc
// functions (write, open, fprintf, ...) because they're dynamically exported.
// On Windows/MinGW those CRT functions are statically linked into the test
// binary and absent from its PE export table, so the generator can't see them
// and every runtime-dependent JIT module fails to materialize with
//   Symbols not found: [ write, open, __mingw_fprintf, stat64i32, ... ]
//
// Fix: take each function's address HERE and hand JitTestHelper a name->addr
// table to install via absoluteSymbols(). The trick that avoids every ABI
// pitfall: we only ever take addresses (never call), and we compile this TU
// with the SAME MinGW headers and flags as runtime/native/cajeta_runtime.c.
// So &fn resolves to the exact entry point the runtime bitcode references —
// no need to know whether mkdir is 1-arg, what struct stat layout is used,
// etc. We register each under the name the JIT actually asks for.
//
// Header remapping must match the runtime's compile so &fn resolves to the
// same symbol the bitcode references. Critically, the runtime bitcode is built
// (src/CMakeLists.txt CAJETA_RT_FLAGS) WITHOUT _FILE_OFFSET_BITS, so its `stat`
// lowers to stat64i32 and `lseek`/`ftruncate` stay 32-bit-offset. We must NOT
// define _FILE_OFFSET_BITS here — doing so remaps &stat/&lseek to the 64-bit
// variants (different symbol + struct layout), so the JIT would call a stat
// with the wrong ABI and every File/Path op returns garbage.
//
// __USE_MINGW_ANSI_STDIO only affects fprintf/snprintf, which we bind by their
// real __mingw_* names below regardless.

#ifdef _WIN32

#ifndef __USE_MINGW_ANSI_STDIO
#define __USE_MINGW_ANSI_STDIO 1
#endif

#include "JitWinSymbols.h"

#include <stddef.h>
#include <io.h>
#include <unistd.h>
#include <sys/stat.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// libmingwex printf-family / strtod: the runtime's fprintf/snprintf/strtod
// calls lower to these under ANSI stdio. Bind them by their real names so we
// don't depend on whether the header exposes fprintf as an inline wrapper.
extern int __mingw_fprintf(FILE*, const char*, ...);
extern int __mingw_snprintf(char*, size_t, const char*, ...);
extern double __mingw_strtod(const char*, char**);

// libgcc stack-probe intrinsic clang emits for functions with large frames.
extern void ___chkstk_ms(void);

// LLVM lowers tan (and sometimes sin/cos) to a combined sincos() libcall.
// sincos is a GNU/libmingwex extension — UCRT/MSVCRT don't export it, so the
// JIT's process-symbol generator can't find it. libmingwex provides it.
extern void sincos(double, double*, double*);
extern void sincosf(float, float*, float*);

// Function-pointer -> void* is not strictly portable C, but is well-defined on
// every Windows/MinGW target; the cast silences -Wpedantic noise.
#define CJ_SYM(jitname, fn) { jitname, (void*) (fn) }

static const CajetaJitWinSym kSymbols[] = {
    CJ_SYM("write",          &write),
    CJ_SYM("read",           &read),
    CJ_SYM("open",           &open),
    CJ_SYM("close",          &close),
    CJ_SYM("lseek",          &lseek),
    CJ_SYM("unlink",         &unlink),
    CJ_SYM("rmdir",          &rmdir),
    CJ_SYM("mkdir",          &mkdir),
    CJ_SYM("getpid",         &getpid),
    CJ_SYM("ftruncate",      &ftruncate),
    CJ_SYM("strdup",         &strdup),
    CJ_SYM("stat64i32",      &stat),
    CJ_SYM("fstat64i32",     &fstat),
    CJ_SYM("__mingw_fprintf",  &__mingw_fprintf),
    CJ_SYM("__mingw_snprintf", &__mingw_snprintf),
    CJ_SYM("__mingw_strtod",   &__mingw_strtod),
    CJ_SYM("___chkstk_ms",     &___chkstk_ms),
    CJ_SYM("sincos",           &sincos),
    CJ_SYM("sincosf",          &sincosf),
};

const CajetaJitWinSym* cajeta_jit_win_symbols(size_t* count) {
    *count = sizeof(kSymbols) / sizeof(kSymbols[0]);
    return kSymbols;
}

#endif  // _WIN32
