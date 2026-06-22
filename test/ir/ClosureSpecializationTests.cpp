//
// Unit 4 (cajeta-ir plan) — closure specialization end-to-end (Approach A).
// A generic method invoked with a directly-supplied non-capturing lambda is
// devirtualized: the call retargets to a specialized instance F<int32>$fn whose
// body calls the lambda DIRECTLY (front-end direct call, visible at O0), the
// closure parameter dropped. Traces spec §3.2-3.4, plan Unit 4.
//

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"

#include <string>

using cajeta_test::CajetaJit;

namespace {

// A generic comparator-apply + a wrapper that passes a directly-written
// non-capturing lambda. run() = applyCmp<int32>(7, 4, x - y) = 3.
const char* kSrc =
    "package test;\n"
    "public class D {\n"
    "    public static int32 applyCmp<T>(T a, T b, (T, T) -> int32 cmp) {\n"
    "        return cmp(a, b);\n"
    "    }\n"
    "    public static int32 run() {\n"
    "        return applyCmp<int32>(7, 4, (int32 x, int32 y) -> x - y);\n"
    "    }\n"
    "}\n";

// A genuine runtime closure: applyCmp is called with `cmp` forwarded from a
// function-typed PARAMETER (not a compile-time-constant closure record), so the
// target is not statically known at that call -> it must stay indirect.
const char* kRuntimeSrc =
    "package test;\n"
    "public class D {\n"
    "    public static int32 applyCmp<T>(T a, T b, (T, T) -> int32 cmp) {\n"
    "        return cmp(a, b);\n"
    "    }\n"
    "    public static int32 viaParam(int32 a, int32 b, (int32, int32) -> int32 cmp) {\n"
    "        return applyCmp<int32>(a, b, cmp);\n"
    "    }\n"
    "    public static int32 run() {\n"
    "        return viaParam(7, 4, (int32 x, int32 y) -> x - y);\n"
    "    }\n"
    "}\n";

// Was `methodName` closure-specialized? A specialized instance's LLVM symbol
// carries the `$spec$` tag right after the method's name+type-args, e.g.
// `...::applyCmp(int32,int32)<int32>$spec$__cajeta_lambda_0`. (Module-wide
// `$spec$` is too coarse — specialization also fires on stdlib generics — so we
// require `methodName` within the same symbol just before the tag.)
bool hasSpecialized(const std::string& ir, const std::string& methodName) {
    size_t pos = 0;
    while ((pos = ir.find("$spec$", pos)) != std::string::npos) {
        size_t from = pos > 200 ? pos - 200 : 0;
        if (ir.substr(from, pos - from).rfind(methodName) != std::string::npos)
            return true;
        pos += 6;
    }
    return false;
}

std::string irFor(const char* src) {
    CajetaJit::Options opts;
    opts.captureIr = true;
    auto jit = CajetaJit::compile(src, "test.D", opts);
    return jit->getModuleIr();
}

} // namespace

// 4.1.1 (result half) — the specialized path runs and is correct.
TEST(ClosureSpecializationTests, directLambdaResultCorrect) {
    auto jit = CajetaJit::compile(kSrc, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 3);
}

// 4.1.1 (IR half) — a specialized instance is emitted and it calls the lambda
// DIRECTLY (no indirect closure dispatch for the comparator parameter).
TEST(ClosureSpecializationTests, specializedInstanceCallsLambdaDirect) {
    std::string ir = irFor(kSrc);
    ASSERT_FALSE(ir.empty());
    // applyCmp<int32> was specialized over the lambda — its body binds cmp to the
    // known fn and calls it directly (BoundClosureField -> emitClosureCall direct
    // path), proven runnable by directLambdaResultCorrect.
    EXPECT_TRUE(hasSpecialized(ir, "applyCmp"))
        << "expected a specialized applyCmp instance\n" << ir;
}

// 4.1.3 — parity: a runtime closure (forwarded from a function-typed parameter,
// not a constant record) is NOT specialized at that call, and still computes
// correctly through the indirect path. (A directly-supplied constant closure —
// even one bound to a function-typed local first — is provably the known target
// and may specialize soundly; only a genuinely runtime target stays indirect.)
TEST(ClosureSpecializationTests, runtimeClosureStaysIndirect) {
    std::string ir = irFor(kRuntimeSrc);
    ASSERT_FALSE(ir.empty());
    EXPECT_FALSE(hasSpecialized(ir, "applyCmp"))
        << "a runtime closure forwarded through a parameter must stay indirect\n" << ir;

    auto jit = CajetaJit::compile(kRuntimeSrc, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 3);
}
