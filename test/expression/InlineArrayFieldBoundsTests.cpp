//
// Static (compile-time) bounds checking for fixed-size inline array fields
// (SSO plan Unit 3; spec 2.1.7). A constant index proven out of range for a
// `T[N]` field is a COMPILE error (the size N is known at compile time); a
// valid constant or any runtime index compiles and follows the existing
// `--bounds` runtime policy unchanged.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

// Wrap a Box (inline int32[4] field) around `body`, executed in Box.run2().
std::string boxSource(const std::string& body) {
    return "package test;\n"
           "public final class Box {\n"
           "    public int32[4] f;\n"
           "    public Box() {}\n"
           "    public int32 run2() {\n"
           "        " + body + "\n"
           "    }\n"
           "}\n"
           "public final class A {\n"
           "    public static int32 run() {\n"
           "        Box b = heap Box();\n"
           "        return b.run2();\n"
           "    }\n"
           "}\n";
}

void expectOutOfBounds(const std::string& body) {
    try {
        CajetaJit::compile(boxSource(body), "test.A");
        FAIL() << "expected a compile-time out-of-bounds error but compile succeeded";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_INLINE_ARRAY_INDEX_OUT_OF_BOUNDS");
        // Message names the offending index and the field/bound.
        EXPECT_NE(e.getMessage().find("f"), std::string::npos) << e.getMessage();
        EXPECT_NE(e.getMessage().find("4"), std::string::npos) << e.getMessage();
    } catch (std::exception& e) {
        FAIL() << "expected cajeta::Exception, got std::exception: " << e.what();
    }
}

} // namespace

// 3.1.1 — constant index == N is a compile error (valid range is 0..N-1).
TEST(InlineArrayFieldBoundsTests, ConstIndexEqualToLengthErrors) {
    expectOutOfBounds("this.f[4] = 1;\n        return 0;");
}

// 3.1.1 — constant index > N is a compile error.
TEST(InlineArrayFieldBoundsTests, ConstIndexAboveLengthErrors) {
    expectOutOfBounds("this.f[9] = 1;\n        return 0;");
}

// 3.1.1 — negative constant index is a compile error.
TEST(InlineArrayFieldBoundsTests, ConstIndexNegativeErrors) {
    expectOutOfBounds("return this.f[-1];");
}

// 3.1.2 — the maximal valid constant index compiles and runs.
TEST(InlineArrayFieldBoundsTests, MaxValidConstIndexCompiles) {
    auto jit = CajetaJit::compile(boxSource(
        "this.f[3] = 77;\n        return this.f[3];"), "test.A");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 77);
}

// 3.1.1 — non-decimal constant indexes go through the same static check.
// LiteralUtils' converters received the RAW token ("0x2", "0b100", "1_0"),
// so a hex/binary index crashed the compile with an uncatchable raw
// std::string throw, and hex letter digits computed as c-'A' (0xA read as
// 0, not 10). These pin every radix and the underscore/L-suffix forms.

TEST(InlineArrayFieldBoundsTests, HexConstIndexWithLetterDigitOutOfBoundsErrors) {
    // 0xA = 10: out of range for f[4] — and a letter digit, so the check
    // only fires if the hex value math is right.
    expectOutOfBounds("this.f[0xA] = 1;\n        return 0;");
}

TEST(InlineArrayFieldBoundsTests, BinaryConstIndexOutOfBoundsErrors) {
    expectOutOfBounds("this.f[0b100] = 1;\n        return 0;");   // 4
}

TEST(InlineArrayFieldBoundsTests, OctalConstIndexInBoundsCompiles) {
    auto jit = CajetaJit::compile(boxSource(
        "this.f[03] = 5;\n        return this.f[03];"), "test.A");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 5);
}



// 3.1.2 — a runtime (non-constant) index compiles (runtime bounds policy
// applies as today, not a compile error).
TEST(InlineArrayFieldBoundsTests, RuntimeIndexCompiles) {
    auto jit = CajetaJit::compile(boxSource(
        "int32 i = 2;\n        this.f[i] = 55;\n        return this.f[i];"), "test.A");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 55);
}
