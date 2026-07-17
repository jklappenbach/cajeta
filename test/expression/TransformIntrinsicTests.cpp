//
// transform-intrinsics plan Unit 1 — combinator recognition + specialization
// gate. Grad/Jit/Vmap/Pmap are recognized as trusted intrinsics (name+signature,
// the Math.*/tryAs precedent); a statically-known lambda argument is handed to
// the transform via closure specialization; a non-specializable target is a
// clear compile error. U1 installs an identity placeholder transform (real
// content lands U3+), so Jit(f)(x) == f(x). Jit's identity is permanent (real
// fusion in U6 is semantics-preserving); the Grad/Vmap placeholder tests are
// superseded by U3/U5. Pmap errors "not yet implemented" from U1 (F8).
//
// Function-typed locals are declared EXPLICITLY (`(int32) -> int32 g = ...`), the
// proven house idiom (LambdaL2Tests) — not `var` (no function-type var-inference
// precedent exists in the corpus).
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

// Wrap a run() body over a combinator applied to a directly-written,
// non-capturing lambda. Mirrors ClosureSpecializationTests' kSrc shape.
std::string src(const std::string& body) {
    return "package test;\n"
           "public final class T {\n"
           "    public static int32 run() {\n"
           "        " + body + "\n"
           "    }\n"
           "}\n";
}

int32_t runI32(const std::string& body) {
    auto jit = CajetaJit::compile(src(body), "test.T");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

// The errorId of the diagnostic compiling `source` throws (cajeta::Exception is
// standalone, not a std::exception — catch it directly, per TypeSigilRetirementTests).
// Empty string if the compile succeeds.
std::string compileErrorId(const std::string& source) {
    try {
        CajetaJit::compile(source, "test.T");
    } catch (cajeta::Exception& e) {
        return e.getErrorId();
    }
    return "";
}

} // namespace

// 1.1.1 + 1.1.4 — Jit is recognized as an intrinsic (not resolved to a user
// method) and its identity placeholder reproduces f; the transformed function is
// stored in a local and called later (§2.4).
TEST(TransformIntrinsics, JitRecognizedIdentity) {
    EXPECT_EQ(runI32("(int32) -> int32 g = Jit((int32 x) -> x * x + 1); return g(3);"), 10);
}

// 1.1.1 — Grad is recognized (intercepted before ordinary resolution).
// U1 placeholder is identity; SUPERSEDED by U3 (Grad -> GradResult<V,G>).
TEST(TransformIntrinsics, GradRecognized_U1Placeholder) {
    EXPECT_EQ(runI32("(int32) -> int32 g = Grad((int32 x) -> x * 2); return g(21);"), 42);
}

// 1.1.1 — Vmap is recognized. U1 placeholder is identity; SUPERSEDED by U5.
TEST(TransformIntrinsics, VmapRecognized_U1Placeholder) {
    EXPECT_EQ(runI32("(int32) -> int32 g = Vmap((int32 x) -> x + 5); return g(37);"), 42);
}

// 1.1.1 + F8 — Pmap is recognized but deliberately unimplemented in v1: the
// recognizer intercepts it and errors "not yet implemented" (proof of
// recognition, not an "unknown method" fall-through).
TEST(TransformIntrinsics, PmapRecognizedNotYetImplemented) {
    EXPECT_EQ(compileErrorId(
        src("(int32) -> int32 g = Pmap((int32 x) -> x + 1); return g(1);")),
        "CAJETA_ERROR_TRANSFORM_PMAP_UNIMPLEMENTED");
}

// 1.1.2 — a non-specializable target (the function arrives through a parameter,
// so it is not a compile-time-known lambda) is a clear, named compile error, not
// a silent degrade. Mirrors ClosureSpecializationTests' kRuntimeSrc.
TEST(TransformIntrinsics, NonSpecializableTargetErrors) {
    const std::string source =
        "package test;\n"
        "public final class T {\n"
        "    public static int32 viaParam((int32) -> int32 f) {\n"
        "        (int32) -> int32 g = Jit(f);\n"
        "        return g(3);\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        return viaParam((int32 x) -> x * x + 1);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(compileErrorId(source), "CAJETA_ERROR_TRANSFORM_NOT_SPECIALIZABLE");
}

// 1.1.3 — the transform operates on f's real monomorphized body: a multi-op f
// through the identity transform reproduces f's observable result.
TEST(TransformIntrinsics, IdentityReproducesMultiOpBody) {
    EXPECT_EQ(runI32("(int32) -> int32 g = Jit((int32 x) -> (x + 1) * (x - 1)); return g(5);"), 24);
}

// 1.1.5 — the same combinator applied to two different lambdas specializes
// independently; no global mutable state bleeds between the two sites.
TEST(TransformIntrinsics, IndependentSpecializationNoCrosstalk) {
    EXPECT_EQ(runI32(
        "(int32) -> int32 a = Jit((int32 x) -> x + 1);\n"
        "        (int32) -> int32 b = Jit((int32 x) -> x * 10);\n"
        "        return a(5) + b(5);"), 56);
}
