// A closure whose parameter is a value type (a Slice) receives it by pointer
// and reads the caller's value, whatever form the argument takes.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.U");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

std::string program(const std::string& arg) {
    return
        "package test;\n"
        "import cajeta.lang.Slice;\n"
        "public final class U {\n"
        "    static int32 sum(Slice<int8> s) {\n"
        "        int32 t = 0;\n"
        "        int64 i = 0;\n"
        "        while (i < s.count()) { t = t + (int32) s[i]; i = i + 1; }\n"
        "        return t * 100 + (int32) s.count();\n"
        "    }\n"
        "    static Slice<int8> win(int8[] b) { return b[2:5]; }\n"
        "    public static int32 run() {\n"
        "        int8[] b = heap int8[8];\n"
        "        b[2] = (int8) 1; b[3] = (int8) 2; b[4] = (int8) 3;\n"
        "        (Slice<int8>) -> int32 f = (Slice<int8> s) -> U.sum(s);\n"
        "        Slice<int8> w = b[2:5];\n"
        "        return f(" + arg + ");\n"
        "    }\n"
        "}\n";
}

} // namespace

TEST(ClosureValueParamTests, sliceLocal) {
    EXPECT_EQ(runI32(program("w")), 603);
}

TEST(ClosureValueParamTests, sliceCallResult) {
    EXPECT_EQ(runI32(program("U.win(b)")), 603);
}

TEST(ClosureValueParamTests, sliceExpression) {
    EXPECT_EQ(runI32(program("b[2:5]")), 603);
}
