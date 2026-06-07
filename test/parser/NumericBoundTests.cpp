//
// Stage 2 of the Vector<T,N> work: numeric category marker bounds.
//
// `<T extends Numeric>` (and Floating / Integral) are satisfied by a
// primitive's FLAGS rather than a CajetaClass parent-chain — the only way to
// bound a type parameter to primitives, which carry no class ancestry. This is
// the reusable substrate the built-in `Vector<T extends Numeric, N>` rides on.
// Verified here independently of Vector via an ordinary user template.
//

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"
#include "../../src/cajeta/error/Exception.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

const char* HOLDER =
    "public class Holder<T extends Numeric> {\n"
    "    T v;\n"
    "    public Holder(T v) { this.v = v; }\n"
    "    public T get() { return this.v; }\n"
    "}\n";

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

// int32 satisfies `T extends Numeric` (INT_FLAG | NUMBER_FLAG).
TEST(NumericBoundTests, intArgSatisfiesNumeric) {
    std::string src = std::string("package test;\n") + HOLDER +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Holder<int32> h = heap Holder<int32>(7);\n"
        "        return h.get();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

// float32 also satisfies Numeric (FLOAT_FLAG | NUMBER_FLAG).
TEST(NumericBoundTests, floatArgSatisfiesNumeric) {
    std::string src = std::string("package test;\n") + HOLDER +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Holder<float32> h = heap Holder<float32>(2.5f);\n"
        "        return (int32) h.get();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 2);
}

// A non-numeric class argument is rejected with the bound error.
TEST(NumericBoundTests, classArgRejectedByNumeric) {
    std::string src = std::string("package test;\n") + HOLDER +
        "public class Thing { public Thing() { return; } }\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Holder<Thing> h = heap Holder<Thing>(heap Thing());\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.D");
        FAIL() << "expected CAJETA_ERROR_TYPE_PARAMETER_BOUND";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_TYPE_PARAMETER_BOUND");
    }
}
