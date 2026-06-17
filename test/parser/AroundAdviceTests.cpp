//
// A5: @Around codegen with @Original typed proceed.
//
// When at least one @Around match exists on a user method, the
// compiler extracts the body into a separately-named LLVM Function
// (canonical + `__original` suffix) and replaces the original
// llvmFunction with a wrapper that calls the @Around advice. The
// advice's `@Original Function<...>` parameter receives a bare
// function pointer to the extracted body — non-capturing function
// values are bare `ptr` at the LLVM level per CajetaFunctionType,
// so passing `llvmOriginalFunction` directly works.
//
// Tests use direct return values rather than the drop-count side
// channel from A4 — @Around can transform the return value, so
// the result IS the observable.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

// Advice that ignores proceed and returns a constant. The user
// method would have returned x*7 = 21, but the wrapper routes
// through the advice which short-circuits to 42. Proves the
// wrapper is intercepting the call (the original body isn't
// reached when the advice doesn't call proceed).
TEST(AroundAdviceTests, aroundSkippingProceedReturnsConstant) {
    auto src =
        "package test;\n"
        "annotation Audited { }\n"
        "@Aspect public class A {\n"
        "    @Around(Audited.class)\n"
        "    public static int32 around((int32) -> int32 proceed, int32 x) {\n"
        "        return 42;\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    @Audited public static int32 work(int32 x) { return x * 7; }\n"
        "    public static int32 run() { return work(3); }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// Advice that delegates to proceed and returns its value
// unchanged. work(3) returns 3*7 = 21; the advice forwards the
// call through proceed(3); the wrapper returns the advice's
// result (21). Verifies the function-typed proceed parameter
// works as an indirect call to the extracted original body.
TEST(AroundAdviceTests, aroundDelegatesToProceedReturnsOriginal) {
    auto src =
        "package test;\n"
        "annotation Audited { }\n"
        "@Aspect public class A {\n"
        "    @Around(Audited.class)\n"
        "    public static int32 around((int32) -> int32 proceed, int32 x) {\n"
        "        return proceed(x);\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    @Audited public static int32 work(int32 x) { return x * 7; }\n"
        "    public static int32 run() { return work(3); }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 21);
}

// Advice transforms the return value. work(3) returns 21; the
// advice returns proceed(3) + 100 = 121. Verifies the advice can
// compose around the call — the canonical @Around pattern.
TEST(AroundAdviceTests, aroundTransformsReturnValue) {
    auto src =
        "package test;\n"
        "annotation Audited { }\n"
        "@Aspect public class A {\n"
        "    @Around(Audited.class)\n"
        "    public static int32 around((int32) -> int32 proceed, int32 x) {\n"
        "        return proceed(x) + 100;\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    @Audited public static int32 work(int32 x) { return x * 7; }\n"
        "    public static int32 run() { return work(3); }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 121);
}

// Advice can read/transform the argument before calling proceed.
// work normally returns x*7; the advice doubles x first, then
// calls proceed(2*x). work(3) → proceed(6) → 6*7 = 42. Confirms
// the wrapper forwards the args correctly and the advice can
// vary them.
TEST(AroundAdviceTests, aroundTransformsArgumentToProceed) {
    auto src =
        "package test;\n"
        "annotation Audited { }\n"
        "@Aspect public class A {\n"
        "    @Around(Audited.class)\n"
        "    public static int32 around((int32) -> int32 proceed, int32 x) {\n"
        "        return proceed(x + x);\n"   // doubles the arg
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    @Audited public static int32 work(int32 x) { return x * 7; }\n"
        "    public static int32 run() { return work(3); }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// Method without @Around has no wrapper. No advice, no extraction,
// no rewrite — just the original body. Baseline regression check
// that the A5 changes only fire when @Around is present.
TEST(AroundAdviceTests, unadvisedMethodHasNoWrapper) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 work(int32 x) { return x * 7; }\n"
        "    public static int32 run() { return work(3); }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 21);
}
