// An interface that declares two methods of one name, told apart by arity or
// by parameter types, dispatches each call to the overload it names, through
// the interface as through the class.

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"

#include <cstdint>

using cajeta_test::CajetaJit;

namespace {
int32_t runI32(const std::string& body) {
    std::string src =
        "package test;\n"
        "interface I {\n"
        "    int32 f(int32 a);\n"
        "    int32 f(int32 a, int32 b);\n"
        "    int32 g(int32 a);\n"
        "    int32 g(int64 a);\n"
        "}\n"
        "final class C implements I {\n"
        "    public C() { }\n"
        "    public int32 f(int32 a) { return 1; }\n"
        "    public int32 f(int32 a, int32 b) { return 2; }\n"
        "    public int32 g(int32 a) { return 3; }\n"
        "    public int32 g(int64 a) { return 4; }\n"
        "}\n"
        "public final class O {\n"
        "    public static int32 run() {\n" + body + "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.O");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}
}

TEST(InterfaceOverloadDispatchTests, arityPicksTheOverloadThroughTheClass) {
    EXPECT_EQ(runI32("        C c = heap C();\n"
                     "        return c.f(1) * 10 + c.f(1, 2);\n"), 12);
}

TEST(InterfaceOverloadDispatchTests, arityPicksTheOverloadThroughTheInterface) {
    EXPECT_EQ(runI32("        I i = heap C();\n"
                     "        return i.f(1) * 10 + i.f(1, 2);\n"), 12);
}

TEST(InterfaceOverloadDispatchTests, parameterTypePicksTheOverloadThroughTheInterface) {
    EXPECT_EQ(runI32("        I i = heap C();\n"
                     "        return i.g(1) * 10 + i.g((int64) 1);\n"), 34);
}
