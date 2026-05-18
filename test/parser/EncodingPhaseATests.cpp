// Phase A tests for @Encoding(EncoderClass):
//
//   - Mutual exclusion with @BigEndian / @LittleEndian / @HostEndian /
//     @Align is enforced at class-prototype build time.
//   - Without those conflicts, requesting @Encoding raises
//     CAJETA_ERROR_ENCODING_NOT_IMPLEMENTED with a clear deferral
//     message pointing at Phase B work.
//   - Classes without @Encoding are unaffected (sanity).

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"
#include "../../src/cajeta/error/Exception.h"

#include <cstdint>

using cajeta_test::CajetaJit;

// Minimal encoder stub — Phase A just verifies the annotation is
// recognized; Phase B does the actual dispatch. We don't even use
// MyEncoder's body, just need the class declaration so `MyEncoder.class`
// resolves at the @Encoding site.
static const char* MIN_ENCODER =
    "public class MyEncoder { public MyEncoder() { return; } }\n";

// @Encoding alone — should hit the Phase-B-not-implemented deferral.
TEST(EncodingPhaseATests, encodingAloneIsDeferred) {
    std::string src = std::string("package test;\n")
        + MIN_ENCODER
        + "@Encoding(MyEncoder.class)\n"
          "public class UserMessage {\n"
          "    public int32 id;\n"
          "    public UserMessage() { this.id = 0; }\n"
          "}\n"
          "public final class D {\n"
          "    public static int32 run() { return 1; }\n"
          "}\n";
    try {
        CajetaJit::compile(src, "test.D");
        FAIL() << "expected CAJETA_ERROR_ENCODING_NOT_IMPLEMENTED";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_ENCODING_NOT_IMPLEMENTED");
    }
}

// @Encoding + @BigEndian on the same class — mutual exclusion fires
// first (before Phase B deferral).
TEST(EncodingPhaseATests, encodingPlusBigEndianRejected) {
    std::string src = std::string("package test;\n")
        + MIN_ENCODER
        + "@Encoding(MyEncoder.class) @BigEndian\n"
          "public class UserMessage {\n"
          "    public int32 id;\n"
          "    public UserMessage() { this.id = 0; }\n"
          "}\n"
          "public final class D {\n"
          "    public static int32 run() { return 1; }\n"
          "}\n";
    try {
        CajetaJit::compile(src, "test.D");
        FAIL() << "expected CAJETA_ERROR_ENCODING_CONFLICT";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_ENCODING_CONFLICT");
    }
}

// @Encoding + @LittleEndian
TEST(EncodingPhaseATests, encodingPlusLittleEndianRejected) {
    std::string src = std::string("package test;\n")
        + MIN_ENCODER
        + "@Encoding(MyEncoder.class) @LittleEndian\n"
          "public class UserMessage {\n"
          "    public int32 id;\n"
          "    public UserMessage() { this.id = 0; }\n"
          "}\n"
          "public final class D {\n"
          "    public static int32 run() { return 1; }\n"
          "}\n";
    try {
        CajetaJit::compile(src, "test.D");
        FAIL() << "expected CAJETA_ERROR_ENCODING_CONFLICT";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_ENCODING_CONFLICT");
    }
}

// @Encoding + @HostEndian
TEST(EncodingPhaseATests, encodingPlusHostEndianRejected) {
    std::string src = std::string("package test;\n")
        + MIN_ENCODER
        + "@Encoding(MyEncoder.class) @HostEndian\n"
          "public class UserMessage {\n"
          "    public int32 id;\n"
          "    public UserMessage() { this.id = 0; }\n"
          "}\n"
          "public final class D {\n"
          "    public static int32 run() { return 1; }\n"
          "}\n";
    try {
        CajetaJit::compile(src, "test.D");
        FAIL() << "expected CAJETA_ERROR_ENCODING_CONFLICT";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_ENCODING_CONFLICT");
    }
}

// Sanity: classes without @Encoding compile normally.
TEST(EncodingPhaseATests, noEncodingNoEffect) {
    auto src =
        "package test;\n"
        "public class Plain {\n"
        "    public int32 v;\n"
        "    public Plain(int32 x) { this.v = x; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Plain p = heap Plain(42);\n"
        "        return p.v;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 42);
}
