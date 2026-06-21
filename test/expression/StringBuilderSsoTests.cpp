//
// StringBuilder small-string optimization (SSO plan Unit 5; spec §3). The
// 64-byte inline buffer must produce byte-identical output to naive
// concatenation across the inline->spill boundary (0,1,63,64,65,200,large),
// and count()/isEmpty()/appendBytes() must behave the same inline and spilled.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

const char* kBuilderProgram =
    "package test;\n"
    "import cajeta.lang.StringBuilder;\n"
    "public final class A {\n"
    "    static #String buildSB(String chunk, int32 n) {\n"
    "        StringBuilder sb = stack StringBuilder();\n"
    "        int32 i = 0;\n"
    "        while (i < n) { sb.append(chunk); i = i + 1; }\n"
    "        return sb.toString();\n"
    "    }\n"
    "    static #String buildNaive(String chunk, int32 n) {\n"
    "        String s = \"\";\n"
    "        int32 i = 0;\n"
    "        while (i < n) { s = s + chunk; i = i + 1; }\n"
    "        return s;\n"
    "    }\n"
    "    static int32 checkSize(String chunk, int32 n) {\n"
    "        return buildSB(chunk, n).equals(buildNaive(chunk, n)) ? 1 : 0;\n"
    "    }\n"
    "    public static int32 c0()   { return checkSize(\"x\", 0); }\n"
    "    public static int32 c1()   { return checkSize(\"x\", 1); }\n"
    "    public static int32 c63()  { return checkSize(\"x\", 63); }\n"
    "    public static int32 c64()  { return checkSize(\"x\", 64); }\n"
    "    public static int32 c65()  { return checkSize(\"x\", 65); }\n"
    "    public static int32 c200() { return checkSize(\"x\", 200); }\n"
    "    public static int32 cBig() { return checkSize(\"abcdefgh\", 4000); }\n"
    "    public static int32 cMeta() {\n"
    "        StringBuilder sb = stack StringBuilder();\n"
    "        if (!sb.isEmpty()) { return 0; }\n"
    "        if (sb.count() != 0) { return 0; }\n"
    "        sb.append(\"hello\");\n"
    "        if (sb.isEmpty()) { return 0; }\n"
    "        if (sb.count() != 5) { return 0; }\n"
    "        int8[] data = heap int8[3];\n"
    "        data[0] = (int8) 65; data[1] = (int8) 66; data[2] = (int8) 67;\n"
    "        sb.appendBytes(data, 0, 3);\n"
    "        if (sb.count() != 8) { return 0; }\n"
    "        int32 i = 0;\n"
    "        while (i < 100) { sb.append(\"0123456789\"); i = i + 1; }\n"
    "        if (sb.count() != 1008) { return 0; }\n"
    "        return 1;\n"
    "    }\n"
    "}\n";

int32_t callOr(CajetaJit* jit, const char* sym) {
    auto fn = jit->lookup<int32_t (*)()>(sym);
    EXPECT_NE(fn, nullptr) << sym;
    if (!fn) return -100;
    try {
        return fn();
    } catch (...) {
        ADD_FAILURE() << sym << " threw";
        return -200;
    }
}

} // namespace

// 5.1.1 / 5.1.2 — every boundary build matches naive concatenation and the
// metadata accessors are correct inline and spilled.
TEST(StringBuilderSsoTests, BoundaryBuildsBitExactAndMetaCorrect) {
    auto jit = CajetaJit::compile(kBuilderProgram, "test.A");
    EXPECT_EQ(callOr(jit.get(), "c0"), 1);
    EXPECT_EQ(callOr(jit.get(), "c1"), 1);
    EXPECT_EQ(callOr(jit.get(), "c63"), 1);
    EXPECT_EQ(callOr(jit.get(), "c64"), 1);
    EXPECT_EQ(callOr(jit.get(), "c65"), 1);
    EXPECT_EQ(callOr(jit.get(), "c200"), 1);
    EXPECT_EQ(callOr(jit.get(), "cBig"), 1);
    EXPECT_EQ(callOr(jit.get(), "cMeta"), 1);
}
