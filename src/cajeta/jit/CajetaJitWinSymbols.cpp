// Windows symbol bridge implementation; see the header for the why. MUST NOT
// define _FILE_OFFSET_BITS: the runtime bitcode is built without it, and
// remapping &stat/&lseek to the 64-bit variants would corrupt every File op.
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
// libmingwex is off the PE export table, so its dirent trio needs binding too.
#include <dirent.h>

// fprintf/snprintf/strtod lower to these under ANSI stdio; bound by real name.
extern "C" int __mingw_fprintf(FILE*, const char*, ...);
extern "C" int __mingw_snprintf(char*, size_t, const char*, ...);
extern "C" double __mingw_strtod(const char*, char**);

// libgcc stack-probe intrinsic clang emits for functions with large frames.
extern "C" void ___chkstk_ms(void);

// LLVM lowers tan to a sincos() libcall, which UCRT/MSVCRT do not export.
extern "C" void sincos(double, double*, double*);
extern "C" void sincosf(float, float*, float*);

// The bf16 conversion builtins, statically linked like libm below. That
// unsigned-short return is mingw's ACTUAL ABI — it hands the bf16 back in AX
// while LLVM reads XMM0 — so the wrappers below must move the result across.
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

// The libm functions Math intrinsics lower to: statically linked and off the PE
// export table, so unbound every math-using JIT module fails to materialize.
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


// The Packages.install bridge. These are DATA, so the real types matter: the
// runtime LOADS THROUGH them, and a wrong width corrupts rather than fails.
extern "C" int32_t (*__cajeta_install_hook)(const char*, int32_t, const char*,
                                            int32_t, int32_t, char*, int32_t,
                                            void*);
extern "C" void* __cajeta_install_ctx;
extern "C" char  __cajeta_install_out[2048];

// cajeta's OWN native families: in libcajeta_lib but off the PE export table
// like libm, so the standalone TLS object and the OptiX entry points need
// binding too. The void() prototypes are deliberate: only addresses are taken.
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

// The sjlj machinery try/catch captures with and __cajeta_throw longjmps to.
// Both live in MSVCRT; declared by hand to dodge the <setjmp.h> macro.
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
    // Unbridged these reach another CRT than the bridged open() owning the fd.
    CJ_SYM("_fstat64",         &::_fstat64),
    CJ_SYM("_lseeki64",        &::_lseeki64),
    CJ_SYM("_chsize_s",        &::_chsize_s),
    CJ_SYM("__mingw_fprintf",  &__mingw_fprintf),
    CJ_SYM("__mingw_snprintf", &__mingw_snprintf),
    CJ_SYM("__mingw_strtod",   &__mingw_strtod),
    CJ_SYM("___chkstk_ms",     &___chkstk_ms),
    CJ_SYM("sincos",           &sincos),
    CJ_SYM("sincosf",          &sincosf),
    // sjlj exception machinery (MSVCRT) — see the extern "C" block above.
    CJ_SYM("_setjmp",          &::_setjmp),
    CJ_SYM("longjmp",          &::longjmp),
    // The truncations go through the XMM0 wrappers, NOT libgcc directly.
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
    // Process-global CRT state: must resolve to the host binary's CRT instance.
    CJ_SYM("_commit",          &::_commit),
    CJ_SYM("getenv",           &::getenv),
    CJ_SYM("_putenv_s",        &::_putenv_s),
    // DATA symbols: their visibility("default") is an ELF mechanism, so on COFF
    // they go unresolved and poison the runtime module for every JIT'd cell.
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
