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
#include <stdint.h>
#include <io.h>
#include <unistd.h>
#include <sys/stat.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// OptiX AS runtime glue (src/cajeta/xpu/nvidia/OptixAccel.cpp). The EMBEDDED
// runtime bitcode's CUDA noun provider references these (the first host-lib
// symbols the embedded runtime calls that aren't themselves embedded — OptiX is
// C++ + needs the driver loader, so it can't live in JIT bitcode). Without
// registering them here, every JIT module on Windows fails to materialize. (On
// Linux the JIT's -rdynamic export table resolves them; this bridge is _WIN32.)
extern int     cajeta_xpu_optix_available(void);
extern int64_t cajeta_xpu_optix_accel_build_aabbs(const float* boxes, unsigned count);
extern int64_t cajeta_xpu_optix_accel_build_triangles(const float* verts,
                                                      unsigned triCount, unsigned stride);
extern void    cajeta_xpu_optix_accel_free(int64_t handle);
extern uint64_t cajeta_xpu_optix_traversable(int64_t handle);
extern uint64_t cajeta_xpu_optix_accel_boxes(int64_t handle);
extern int      cajeta_xpu_optix_launch(const char* ptx, uint64_t ptxLen,
                                        const char* raygenName, const char* isName,
                                        const char* anyhitName, const char* missName,
                                        const void* paramsHost, uint64_t paramsLen,
                                        unsigned width);
extern int      cajeta_xpu_optix_launch_tri(const char* ptx, uint64_t ptxLen,
                                            const char* raygenName,
                                            const char* closesthitName,
                                            const char* missName,
                                            const void* paramsHost, uint64_t paramsLen,
                                            unsigned width);

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

// libm math functions the stdlib's Math intrinsics + float ops lower to. They
// are statically linked from libm/libmingwex but absent from the host PE export
// table, so the JIT's process-symbol generator can't see them — every
// math-using JIT module then fails to materialize ("Symbols not found: [fabsf]"
// was the first miss, which cascaded into a whole-module materialization
// failure and failed every Windows release test). Declared extern (not via
// <math.h>) to dodge mingw header macro/inline expansion, same as sincos.
extern float  fabsf(float);       extern double fabs(double);
extern float  sqrtf(float);       extern double sqrt(double);
extern float  powf(float, float); extern double pow(double, double);
extern float  expf(float);        extern double exp(double);
extern float  logf(float);        extern double log(double);
extern float  log10f(float);      extern double log10(double);
extern float  log2f(float);       extern double log2(double);
extern float  sinf(float);        extern double sin(double);
extern float  cosf(float);        extern double cos(double);
extern float  tanf(float);        extern double tan(double);
extern float  ceilf(float);       extern double ceil(double);
extern float  floorf(float);      extern double floor(double);
extern float  roundf(float);      extern double round(double);
extern float  truncf(float);      extern double trunc(double);
extern float  fmodf(float, float); extern double fmod(double, double);
extern float  fminf(float, float); extern double fmin(double, double);
extern float  fmaxf(float, float); extern double fmax(double, double);

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
    // libm — see the extern block above for why these need binding.
    CJ_SYM("fabsf",  &fabsf),   CJ_SYM("fabs",   &fabs),
    CJ_SYM("sqrtf",  &sqrtf),   CJ_SYM("sqrt",   &sqrt),
    CJ_SYM("powf",   &powf),    CJ_SYM("pow",    &pow),
    CJ_SYM("expf",   &expf),    CJ_SYM("exp",    &exp),
    CJ_SYM("logf",   &logf),    CJ_SYM("log",    &log),
    CJ_SYM("log10f", &log10f),  CJ_SYM("log10",  &log10),
    CJ_SYM("log2f",  &log2f),   CJ_SYM("log2",   &log2),
    CJ_SYM("sinf",   &sinf),    CJ_SYM("sin",    &sin),
    CJ_SYM("cosf",   &cosf),    CJ_SYM("cos",    &cos),
    CJ_SYM("tanf",   &tanf),    CJ_SYM("tan",    &tan),
    CJ_SYM("ceilf",  &ceilf),   CJ_SYM("ceil",   &ceil),
    CJ_SYM("floorf", &floorf),  CJ_SYM("floor",  &floor),
    CJ_SYM("roundf", &roundf),  CJ_SYM("round",  &round),
    CJ_SYM("truncf", &truncf),  CJ_SYM("trunc",  &trunc),
    CJ_SYM("fmodf",  &fmodf),   CJ_SYM("fmod",   &fmod),
    CJ_SYM("fminf",  &fminf),   CJ_SYM("fmin",   &fmin),
    CJ_SYM("fmaxf",  &fmaxf),   CJ_SYM("fmax",   &fmax),
    // Stateful CRT functions that maintain process-global tables. These MUST
    // resolve to the same CRT instance as the host test binary (and as the
    // open/read/write above) — otherwise the JIT'd code operates on a
    // different table than the host. Without binding them here, LLJIT's
    // process-symbol generator resolves them independently (e.g. against
    // ucrtbase.dll), and:
    //   _commit  — gets an fd opened in the runtime's CRT, sees it as foreign,
    //              and fast-fails (0xC0000409) — crashed File.writeAllBytes.
    //   getenv / _putenv_s — read/write a different environment block, so
    //              host-set vars are invisible to JIT'd System.env.get.
    CJ_SYM("_commit",          &_commit),
    CJ_SYM("getenv",           &getenv),
    CJ_SYM("_putenv_s",        &_putenv_s),
    // OptiX AS glue (see the extern block above): the embedded runtime's CUDA
    // noun provider calls these, so every JIT module needs them resolvable.
    CJ_SYM("cajeta_xpu_optix_available",              &cajeta_xpu_optix_available),
    CJ_SYM("cajeta_xpu_optix_accel_build_aabbs",      &cajeta_xpu_optix_accel_build_aabbs),
    CJ_SYM("cajeta_xpu_optix_accel_build_triangles",  &cajeta_xpu_optix_accel_build_triangles),
    CJ_SYM("cajeta_xpu_optix_accel_free",             &cajeta_xpu_optix_accel_free),
    CJ_SYM("cajeta_xpu_optix_traversable",            &cajeta_xpu_optix_traversable),
    CJ_SYM("cajeta_xpu_optix_accel_boxes",            &cajeta_xpu_optix_accel_boxes),
    CJ_SYM("cajeta_xpu_optix_launch",                 &cajeta_xpu_optix_launch),
    CJ_SYM("cajeta_xpu_optix_launch_tri",             &cajeta_xpu_optix_launch_tri),
};

const CajetaJitWinSym* cajeta_jit_win_symbols(size_t* count) {
    *count = sizeof(kSymbols) / sizeof(kSymbols[0]);
    return kSymbols;
}

#endif  // _WIN32
