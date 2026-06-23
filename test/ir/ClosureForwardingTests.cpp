//
// Phase B Unit 1 — forwarding-chain specialization.
// When a specialized instance forwards its (now-known) comparator to another
// generic callee, that forwarded call specializes too, so the chain
// devirtualizes end-to-end. The motivating real case is Sort.binarySearch,
// which forwards its comparator to lowerBound/upperBound. Traces phase-b §2.
//

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"

#include <string>

using cajeta_test::CajetaJit;

namespace {

// outer<T> forwards cmp to inner<T>; run() supplies a directly-written
// non-capturing lambda. run() = outer(7,4, x-y) = inner(7,4,cmp) = 3.
const char* kFwd =
    "package test;\n"
    "public class D {\n"
    "    public static int32 inner<T>(T a, T b, (T, T) -> int32 cmp) {\n"
    "        return cmp(a, b);\n"
    "    }\n"
    "    public static int32 outer<T>(T a, T b, (T, T) -> int32 cmp) {\n"
    "        return inner<T>(a, b, cmp);\n"
    "    }\n"
    "    public static int32 run() {\n"
    "        return outer<int32>(7, 4, (int32 x, int32 y) -> x - y);\n"
    "    }\n"
    "}\n";

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

// 1.1.1 — both the forwarding caller AND the forwarded callee specialize.
TEST(ClosureForwardingTests, forwardedCalleeSpecializes) {
    std::string ir = irFor(kFwd);
    ASSERT_FALSE(ir.empty());
    EXPECT_TRUE(hasSpecialized(ir, "outer"))
        << "the forwarding caller should specialize\n" << ir;
    EXPECT_TRUE(hasSpecialized(ir, "inner"))
        << "the forwarded callee should specialize too\n" << ir;
}

// 1.1.* — result is correct through the fully-devirtualized chain.
TEST(ClosureForwardingTests, forwardingResultCorrect) {
    auto jit = CajetaJit::compile(kFwd, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 3);
}
