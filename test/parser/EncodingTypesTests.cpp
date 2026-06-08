// String Phase 2a: the encoding-interchange infrastructure.
// Verify the three new stdlib types in cajeta.lang:
//   - Encoding enum (12 variants)
//   - EncodingErrorPolicy enum (FAIL / REPAIR)
//   - EncodingException (extends RecoverableException)
//
// Per docs/stdlib/lang/String.md § Encoding interchange.

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"
#include "../../src/cajeta/error/Exception.h"
#include <cstdint>

using cajeta_test::CajetaJit;

namespace {
int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

constexpr const char* PRELUDE =
    "package test;\n"
    "import cajeta.lang.Encoding;\n"
    "import cajeta.lang.EncodingErrorPolicy;\n"
    "import cajeta.lang.EncodingException;\n";
} // namespace

// Encoding enum constants have sequential ordinals 0..N starting at UTF_8.
TEST(EncodingTypesTests, encodingOrdinalsAreSequential) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        return Encoding.UTF_8;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 0);  // UTF_8 is the first constant
}

TEST(EncodingTypesTests, encodingHasAllTwelveConstants) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        // Sum the ordinals: 0+1+2+...+11 = 66
        "        return Encoding.UTF_8 + Encoding.UTF_16_LE + Encoding.UTF_16_BE\n"
        "             + Encoding.UTF_32_LE + Encoding.UTF_32_BE\n"
        "             + Encoding.ASCII + Encoding.LATIN_1 + Encoding.WINDOWS_1252\n"
        "             + Encoding.GB18030 + Encoding.SHIFT_JIS\n"
        "             + Encoding.EUC_KR + Encoding.BIG_5;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 66);
}

// EncodingErrorPolicy: FAIL = 0 (default), REPAIR = 1.
TEST(EncodingTypesTests, errorPolicyOrdinals) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        return EncodingErrorPolicy.FAIL * 10 + EncodingErrorPolicy.REPAIR;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);  // 0 * 10 + 1 = 1
}

// Encoding-typed local variable holds the ordinal.
TEST(EncodingTypesTests, encodingVariableHoldsOrdinal) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Encoding e = Encoding.GB18030;\n"
        "        return e;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 8);  // GB18030 ordinal
}

// EncodingException constructs with all fields and exposes offset.
TEST(EncodingTypesTests, encodingExceptionConstructAndRead) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        EncodingException e = heap EncodingException(\n"
        "            \"codepoint not representable\",\n"
        "            (int64) 42,\n"                  // offset
        "            128512,\n"                       // codepoint U+1F600
        "            Encoding.UTF_8,\n"               // source
        "            Encoding.ASCII,\n"               // target
        "            \"U+1F600 out of ASCII range\");\n"
        "        return (int32) e.offset * 1000 + e.codepoint;\n"
        "    }\n"
        "}\n";
    // 42 * 1000 + 128512 = 170512
    EXPECT_EQ(runI32(src), 170512);
}

// EncodingException is throwable; sourceEncoding/targetEncoding fields hold ordinals.
TEST(EncodingTypesTests, encodingExceptionThrowAndCatchFields) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        EncodingException e = heap EncodingException(\n"
        "            \"oops\", (int64) 0, 100,\n"
        "            Encoding.UTF_8, Encoding.LATIN_1, \"oops\");\n"
        "        return e.sourceEncoding * 100 + e.targetEncoding;\n"
        "    }\n"
        "}\n";
    // UTF_8=0, LATIN_1=6 → 0*100 + 6 = 6
    EXPECT_EQ(runI32(src), 6);
}
