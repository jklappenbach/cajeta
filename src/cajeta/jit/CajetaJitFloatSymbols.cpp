//
// fp128 compiler-rt builtin bridge implementation. See header for the why.
// Only addresses are taken (never calls), so the bare `void(void)` decls are
// purely for linkage — the real signatures (long double / __float128 args)
// don't matter for `&fn`. Gated to APPLE: that's the only target where these
// helpers aren't dynamically exported from the host process.
//
#include "cajeta/jit/CajetaJitFloatSymbols.h"

#ifdef __APPLE__

// compiler-rt fp128 (tf) soft-float helpers. Declared with a placeholder
// `void(void)` signature solely to take their address; the linker resolves
// them from libclang_rt.osx.a (clang auto-links the builtins archive).
extern "C" {
    // double/float <-> fp128
    void __extenddftf2(void);
    void __extendsftf2(void);
    void __trunctfdf2(void);
    void __trunctfsf2(void);
    // fp128 -> signed/unsigned integer
    void __fixtfsi(void);
    void __fixtfdi(void);
    void __fixunstfsi(void);
    void __fixunstfdi(void);
    // signed/unsigned integer -> fp128
    void __floatsitf(void);
    void __floatditf(void);
    void __floatunsitf(void);
    void __floatunditf(void);
    // fp128 arithmetic
    void __addtf3(void);
    void __subtf3(void);
    void __multf3(void);
    void __divtf3(void);
    void __negtf2(void);
    // fp128 comparison
    void __eqtf2(void);
    void __netf2(void);
    void __getf2(void);
    void __letf2(void);
    void __lttf2(void);
    void __gttf2(void);
    void __unordtf2(void);
    void __cmptf2(void);
}

namespace cajeta::jit {

#define CJ_FSYM(fn) { #fn, reinterpret_cast<void*>(fn) }

static const JitFloatSym kFloatSymbols[] = {
    CJ_FSYM(__extenddftf2),
    CJ_FSYM(__extendsftf2),
    CJ_FSYM(__trunctfdf2),
    CJ_FSYM(__trunctfsf2),
    CJ_FSYM(__fixtfsi),
    CJ_FSYM(__fixtfdi),
    CJ_FSYM(__fixunstfsi),
    CJ_FSYM(__fixunstfdi),
    CJ_FSYM(__floatsitf),
    CJ_FSYM(__floatditf),
    CJ_FSYM(__floatunsitf),
    CJ_FSYM(__floatunditf),
    CJ_FSYM(__addtf3),
    CJ_FSYM(__subtf3),
    CJ_FSYM(__multf3),
    CJ_FSYM(__divtf3),
    CJ_FSYM(__negtf2),
    CJ_FSYM(__eqtf2),
    CJ_FSYM(__netf2),
    CJ_FSYM(__getf2),
    CJ_FSYM(__letf2),
    CJ_FSYM(__lttf2),
    CJ_FSYM(__gttf2),
    CJ_FSYM(__unordtf2),
    CJ_FSYM(__cmptf2),
};

const JitFloatSym* floatJitSymbols(size_t* count) {
    *count = sizeof(kFloatSymbols) / sizeof(kFloatSymbols[0]);
    return kFloatSymbols;
}

} // namespace cajeta::jit

#else  // !__APPLE__

namespace cajeta::jit {
    const JitFloatSym* floatJitSymbols(size_t* count) {
        *count = 0;
        return nullptr;
    }
} // namespace cajeta::jit

#endif  // __APPLE__
