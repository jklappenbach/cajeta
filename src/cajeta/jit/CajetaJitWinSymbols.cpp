//
// Windows symbol bridge implementation. See header for the why. Port of
// test/jit/JitWinSymbols.c into the src tree (C++ TU so it is picked up by the
// src `*.cpp` glob; only addresses are taken, never calls, so C-vs-C++ makes
// no ABI difference). MUST NOT define _FILE_OFFSET_BITS — the runtime bitcode
// is built without it, so `stat` lowers to stat64i32 and lseek/ftruncate stay
// 32-bit-offset; defining it here would remap &stat/&lseek to the 64-bit
// variants (different symbol + struct layout) and corrupt every File/Path op.
//
#include "cajeta/jit/CajetaJitWinSymbols.h"

#ifdef _WIN32

#ifndef __USE_MINGW_ANSI_STDIO
#define __USE_MINGW_ANSI_STDIO 1
#endif

#include <io.h>
#include <unistd.h>
#include <sys/stat.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// libmingwex printf-family / strtod: the runtime's fprintf/snprintf/strtod
// calls lower to these under ANSI stdio. Bind them by their real names so we
// don't depend on whether the header exposes fprintf as an inline wrapper.
extern "C" int __mingw_fprintf(FILE*, const char*, ...);
extern "C" int __mingw_snprintf(char*, size_t, const char*, ...);
extern "C" double __mingw_strtod(const char*, char**);

// libgcc stack-probe intrinsic clang emits for functions with large frames.
extern "C" void ___chkstk_ms(void);

// LLVM lowers tan (and sometimes sin/cos) to a combined sincos() libcall.
// sincos is a GNU/libmingwex extension — UCRT/MSVCRT don't export it.
extern "C" void sincos(double, double*, double*);
extern "C" void sincosf(float, float*, float*);

namespace cajeta::jit {

#define CJ_SYM(jitname, fn) { jitname, reinterpret_cast<void*>(fn) }

static const JitWinSym kSymbols[] = {
    CJ_SYM("write",            &::write),
    CJ_SYM("read",             &::read),
    CJ_SYM("open",             &::open),
    CJ_SYM("close",            &::close),
    CJ_SYM("lseek",            &::lseek),
    CJ_SYM("unlink",           &::unlink),
    CJ_SYM("rmdir",            &::rmdir),
    CJ_SYM("mkdir",            &::mkdir),
    CJ_SYM("getpid",           &::getpid),
    CJ_SYM("ftruncate",        &::ftruncate),
    CJ_SYM("strdup",           &::strdup),
    CJ_SYM("stat64i32",        &::stat),
    CJ_SYM("fstat64i32",       &::fstat),
    CJ_SYM("__mingw_fprintf",  &__mingw_fprintf),
    CJ_SYM("__mingw_snprintf", &__mingw_snprintf),
    CJ_SYM("__mingw_strtod",   &__mingw_strtod),
    CJ_SYM("___chkstk_ms",     &___chkstk_ms),
    CJ_SYM("sincos",           &sincos),
    CJ_SYM("sincosf",          &sincosf),
    // Stateful CRT functions that maintain process-global tables — must resolve
    // to the same CRT instance as the host binary (see JitWinSymbols.c).
    CJ_SYM("_commit",          &::_commit),
    CJ_SYM("getenv",           &::getenv),
    CJ_SYM("_putenv_s",        &::_putenv_s),
};

const JitWinSym* winJitSymbols(size_t* count) {
    *count = sizeof(kSymbols) / sizeof(kSymbols[0]);
    return kSymbols;
}

} // namespace cajeta::jit

#else  // !_WIN32

namespace cajeta::jit {
    const JitWinSym* winJitSymbols(size_t* count) {
        *count = 0;
        return nullptr;
    }
} // namespace cajeta::jit

#endif  // _WIN32
