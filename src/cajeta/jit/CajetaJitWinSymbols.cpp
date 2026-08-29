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
#include <stdint.h>   // int32_t — the install-bridge declarations below
#include <unistd.h>
#include <sys/stat.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
// opendir/readdir/closedir for __cajeta_path_list — mingw-w64 ships them in
// libmingwex (statically linked, absent from the PE export table), so they
// need the same address-binding as the libm family below.
#include <dirent.h>

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

// compiler-rt/libgcc bfloat16 conversion builtins the CPU-kernel lowering emits
// for bf16 arithmetic (float/double <-> bf16). libgcc provides them but, like
// the libm family below, they're statically linked and absent from the host PE
// export table, so the JIT's process generator can't see them — every bf16
// kernel then fails to materialize ("Symbols not found: [ __truncsfbf2 ]").
// The unsigned-short return here is NOT a placeholder: it is mingw's actual
// ABI, and binding these directly was silently wrong.
//
// LLVM lowers `fptrunc float to bfloat` to `call __truncsfbf2` followed by
// `pextrw $0, %xmm0` — it reads the bf16 back out of XMM0. mingw's libgcc
// returns it in AX instead (every return path in truncsfbf2.o ends
// `mov %edx,%eax; shl $0xf,%eax; ret`), and never touches XMM0. Linux libgcc
// ends the same paths with `movd %ebx,%xmm0`, which is why this only bites on
// Windows.
//
// The consequence is silent wrong data, not a crash: XMM0 still holds the
// INPUT float, so pextrw takes that value's low 16 bits. Every float with a
// zero low half — 1.0f, 2.0f, 10.0f, every constant in a typical kernel —
// converts to bf16 0x0000, i.e. 0.0. XpuVectorDeviceTests.bfloat16RunsOnCpu
// computes (1,2,3,4)+(10,20,30,40) and read back all zeroes.
//
// So bind wrappers that call libgcc for the arithmetic (its rounding is
// correct) and move the result into XMM0 the way LLVM expects, by returning a
// float whose low 16 bits carry the bf16 pattern.
extern "C" unsigned short __truncsfbf2(float);
extern "C" unsigned short __truncdfbf2(double);
extern "C" float          __extendbfsf2(unsigned short);

static float cajetaTruncSfBf2(float x) {
    unsigned int bits = __truncsfbf2(x);   // mingw hands this back in AX
    float out;
    __builtin_memcpy(&out, &bits, sizeof out);
    return out;                            // ...and this puts it in XMM0
}

static float cajetaTruncDfBf2(double x) {
    unsigned int bits = __truncdfbf2(x);
    float out;
    __builtin_memcpy(&out, &bits, sizeof out);
    return out;
}

// libm math functions the stdlib's Math intrinsics + float ops lower to. These
// are statically linked from libm/libmingwex but absent from the host PE export
// table, so the JIT's process-symbol generator can't see them — every
// math-using JIT module then fails to materialize ("Symbols not found: [fabsf]"
// was the first miss, which cascaded to a whole-module materialization failure
// and failed every Windows release test). Declared extern "C" (not via <math.h>)
// to dodge mingw header macro/inline expansion, same as sincos above. Bind by
// address below. POSIX hosts export these from libm, so this is Windows-only.
extern "C" {
    float  fabsf(float);     double fabs(double);
    float  sqrtf(float);     double sqrt(double);
    float  powf(float, float); double pow(double, double);
    float  expf(float);      double exp(double);
    float  logf(float);      double log(double);
    float  log10f(float);    double log10(double);
    float  log2f(float);     double log2(double);
    float  sinf(float);      double sin(double);
    float  cosf(float);      double cos(double);
    float  tanf(float);      double tan(double);
    float  ceilf(float);     double ceil(double);
    float  floorf(float);    double floor(double);
    float  roundf(float);    double round(double);
    float  truncf(float);    double trunc(double);
    float  fmodf(float, float); double fmod(double, double);
    float  fminf(float, float); double fmin(double, double);
    float  fmaxf(float, float); double fmax(double, double);
}


// The Packages.install bridge, defined in KernelSession.cpp. These are DATA,
// so unlike everything else in this file the real types matter: the JIT'd
// runtime LOADS THROUGH them, and registering an address of the wrong width
// or shape would corrupt rather than fail to resolve. Declared exactly as
// defined there.
extern "C" int32_t (*__cajeta_install_hook)(const char*, int32_t, const char*,
                                            int32_t, int32_t, char*, int32_t,
                                            void*);
extern "C" void* __cajeta_install_ctx;
extern "C" char  __cajeta_install_out[2048];

// cajeta's OWN native families that live in libcajeta_lib but are absent from
// the PE export table, so the process-symbol generator cannot see them — the
// same class as the libm/dirent bindings above, rediscovered one family at a
// time (v0.16.0 re-cut: dirent; v0.21.0 dry-run: every DAP/kernel test failed
// materializing the runtime with "Symbols not found: [ __cajeta_tls_*,
// cajeta_xpu_optix_* ]"). The TLS engine is a standalone native object (kept
// out of the JIT bitcode so OpenSSL headers stay out of it — src/CMakeLists
// ~623) and the OptiX entry points always exist (real or stub, OptixAccel.cpp)
// — both comment "the JIT resolves them via its process-symbol generator",
// which is true only on hosts that export them. Prototypes are deliberately
// void(): only addresses are taken, never calls (same rationale as the file
// header's C-vs-C++ note).
extern "C" void __cajeta_tls_conn_new();
extern "C" void __cajeta_tls_ctx_add_trust_pem();
extern "C" void __cajeta_tls_ctx_free();
extern "C" void __cajeta_tls_ctx_new();
extern "C" void __cajeta_tls_ctx_set_alpn_select();
extern "C" void __cajeta_tls_ctx_set_verify();
extern "C" void __cajeta_tls_ctx_use_cert_key_pem();
extern "C" void __cajeta_tls_ctx_use_system_trust();
extern "C" void __cajeta_tls_feed_ciphertext();
extern "C" void __cajeta_tls_free();
extern "C" void __cajeta_tls_get_alpn();
extern "C" void __cajeta_tls_handshake_step();
extern "C" void __cajeta_tls_pending_ciphertext();
extern "C" void __cajeta_tls_pull_ciphertext();
extern "C" void __cajeta_tls_read_plaintext();
extern "C" void __cajeta_tls_set_alpn();
extern "C" void __cajeta_tls_set_sni();
extern "C" void __cajeta_tls_set_verify_host();
extern "C" void __cajeta_tls_shutdown();
extern "C" void __cajeta_tls_verify_result();
extern "C" void __cajeta_tls_write_plaintext();
extern "C" void cajeta_xpu_optix_available();
extern "C" void cajeta_xpu_optix_context();
extern "C" void cajeta_xpu_optix_cuda_context();
extern "C" void cajeta_xpu_optix_accel_build_aabbs();
extern "C" void cajeta_xpu_optix_accel_build_triangles();
extern "C" void cajeta_xpu_optix_traversable();
extern "C" void cajeta_xpu_optix_accel_boxes();
extern "C" void cajeta_xpu_optix_accel_free();
extern "C" void cajeta_xpu_optix_launch();
extern "C" void cajeta_xpu_optix_launch_tri();

// sjlj exception machinery. Codegen's try/catch and the runtime bitcode's
// session guard capture with `_setjmp(frame, NULL)` on COFF (non-unwinding —
// ExcFrameSetjmp.h / cajeta_rt_session.c), and __cajeta_throw longjmps. Both
// live in MSVCRT, whose PE exports the process generator cannot see, so bind
// them by address like the libm family above. Declared by hand (not via
// <setjmp.h>) to dodge the header's setjmp macro; addresses only, never
// called from here.
extern "C" int _setjmp(void*, void*);
extern "C" void longjmp(void*, int);

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
    // dirent (libmingwex) — __cajeta_path_list's directory walk.
    CJ_SYM("opendir",          &::opendir),
    CJ_SYM("readdir",          &::readdir),
    CJ_SYM("closedir",         &::closedir),
    CJ_SYM("stat64i32",        &::stat),
    CJ_SYM("fstat64i32",       &::fstat),
    CJ_SYM("__mingw_fprintf",  &__mingw_fprintf),
    CJ_SYM("__mingw_snprintf", &__mingw_snprintf),
    CJ_SYM("__mingw_strtod",   &__mingw_strtod),
    CJ_SYM("___chkstk_ms",     &___chkstk_ms),
    CJ_SYM("sincos",           &sincos),
    CJ_SYM("sincosf",          &sincosf),
    // sjlj exception machinery (MSVCRT) — see the extern "C" block above.
    CJ_SYM("_setjmp",          &::_setjmp),
    CJ_SYM("longjmp",          &::longjmp),
    // bf16 conversion builtins — see the extern "C" block above.
    // The two truncations go through the XMM0 wrappers, NOT libgcc directly.
    // __extendbfsf2 is bound as-is: LLVM expands `fpext bfloat to float`
    // inline as a 16-bit shift and never emits the call, so there is no
    // observed ABI to correct here.
    CJ_SYM("__truncsfbf2",     &cajetaTruncSfBf2),
    CJ_SYM("__truncdfbf2",     &cajetaTruncDfBf2),
    CJ_SYM("__extendbfsf2",    &__extendbfsf2),
    // libm — see the extern "C" block above for why these need binding.
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
    // cajeta native families invisible to COFF process lookup — see above.
    CJ_SYM("__cajeta_tls_conn_new", &__cajeta_tls_conn_new),
    CJ_SYM("__cajeta_tls_ctx_add_trust_pem", &__cajeta_tls_ctx_add_trust_pem),
    CJ_SYM("__cajeta_tls_ctx_free", &__cajeta_tls_ctx_free),
    CJ_SYM("__cajeta_tls_ctx_new", &__cajeta_tls_ctx_new),
    CJ_SYM("__cajeta_tls_ctx_set_alpn_select", &__cajeta_tls_ctx_set_alpn_select),
    CJ_SYM("__cajeta_tls_ctx_set_verify", &__cajeta_tls_ctx_set_verify),
    CJ_SYM("__cajeta_tls_ctx_use_cert_key_pem", &__cajeta_tls_ctx_use_cert_key_pem),
    CJ_SYM("__cajeta_tls_ctx_use_system_trust", &__cajeta_tls_ctx_use_system_trust),
    CJ_SYM("__cajeta_tls_feed_ciphertext", &__cajeta_tls_feed_ciphertext),
    CJ_SYM("__cajeta_tls_free", &__cajeta_tls_free),
    CJ_SYM("__cajeta_tls_get_alpn", &__cajeta_tls_get_alpn),
    CJ_SYM("__cajeta_tls_handshake_step", &__cajeta_tls_handshake_step),
    CJ_SYM("__cajeta_tls_pending_ciphertext", &__cajeta_tls_pending_ciphertext),
    CJ_SYM("__cajeta_tls_pull_ciphertext", &__cajeta_tls_pull_ciphertext),
    CJ_SYM("__cajeta_tls_read_plaintext", &__cajeta_tls_read_plaintext),
    CJ_SYM("__cajeta_tls_set_alpn", &__cajeta_tls_set_alpn),
    CJ_SYM("__cajeta_tls_set_sni", &__cajeta_tls_set_sni),
    CJ_SYM("__cajeta_tls_set_verify_host", &__cajeta_tls_set_verify_host),
    CJ_SYM("__cajeta_tls_shutdown", &__cajeta_tls_shutdown),
    CJ_SYM("__cajeta_tls_verify_result", &__cajeta_tls_verify_result),
    CJ_SYM("__cajeta_tls_write_plaintext", &__cajeta_tls_write_plaintext),
    CJ_SYM("cajeta_xpu_optix_available", &cajeta_xpu_optix_available),
    CJ_SYM("cajeta_xpu_optix_context", &cajeta_xpu_optix_context),
    CJ_SYM("cajeta_xpu_optix_cuda_context", &cajeta_xpu_optix_cuda_context),
    CJ_SYM("cajeta_xpu_optix_accel_build_aabbs", &cajeta_xpu_optix_accel_build_aabbs),
    CJ_SYM("cajeta_xpu_optix_accel_build_triangles", &cajeta_xpu_optix_accel_build_triangles),
    CJ_SYM("cajeta_xpu_optix_traversable", &cajeta_xpu_optix_traversable),
    CJ_SYM("cajeta_xpu_optix_accel_boxes", &cajeta_xpu_optix_accel_boxes),
    CJ_SYM("cajeta_xpu_optix_accel_free", &cajeta_xpu_optix_accel_free),
    CJ_SYM("cajeta_xpu_optix_launch", &cajeta_xpu_optix_launch),
    CJ_SYM("cajeta_xpu_optix_launch_tri", &cajeta_xpu_optix_launch_tri),
    // Stateful CRT functions that maintain process-global tables — must resolve
    // to the same CRT instance as the host binary (see JitWinSymbols.c).
    CJ_SYM("_commit",          &::_commit),
    CJ_SYM("getenv",           &::getenv),
    CJ_SYM("_putenv_s",        &::_putenv_s),
    // The Packages.install bridge — DATA symbols, not functions, and the only
    // host-side state the embedded runtime reads directly. They are defined in
    // KernelSession.cpp with visibility("default"), which is an ELF mechanism:
    // on COFF a PE exports nothing regardless, so the process generator cannot
    // see them and cajeta_rt_session.c's references go unresolved. Because that
    // TU is part of the STANDARD embedded runtime, the failure is not confined
    // to notebook tests — it poisons the runtime module for every JIT'd cell,
    // which is how one missing bridge produced 82 failures across suites as
    // unrelated as Protobuf, Avro, Vmap and Varargs.
    CJ_SYM("__cajeta_install_hook", &__cajeta_install_hook),
    CJ_SYM("__cajeta_install_ctx",  &__cajeta_install_ctx),
    CJ_SYM("__cajeta_install_out",  &__cajeta_install_out),
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
