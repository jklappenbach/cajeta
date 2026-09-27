// A Slice returned by a call is used directly: indexed and asked its count
// without binding it to a local first.

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

const char* kBox =
    "package test;\n"
    "import cajeta.lang.Slice;\n"
    "public final class Box {\n"
    "    int8[] b;\n"
    "    public Box() { this.b = heap int8[8]; this.b[3] = (int8) 7; }\n"
    "    public Slice<int8> win() { return this.b[2:6]; }\n"
    "}\n";

} // namespace

TEST(SliceCallResultTests, indexACallResultDirectly) {
    auto src = std::string(kBox) +
        "public final class U {\n"
        "    public static int32 run() {\n"
        "        Box x = heap Box();\n"
        "        Slice<int8> w = x.win();\n"
        "        return (int32) x.win()[1] * 10 + (int32) w[1];\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 77);
}

TEST(SliceCallResultTests, countACallResultDirectly) {
    auto src = std::string(kBox) +
        "public final class U {\n"
        "    public static int32 run() {\n"
        "        Box x = heap Box();\n"
        "        return (int32) x.win().count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 4);
}
