// String Phase 2b: `cajeta.lang.String` becomes a real class (replacing
// the opaque i8* primitive alias). Phase 2b-α pins the class skeleton —
// fields exist, can be instantiated, fields read back. String-literal
// codegen and method surface come in later phases.
//
// Per cajeta-docs/stdlib/lang/String.md § Storage model:
//
//     class String {
//         int8[] bytes;          // UTF-8 payload (owner of the bytes
//                                // when mode = 0; borrowed view when 1)
//         int32  byteLength;     // length in bytes
//         int32  mode;           // 0 = owned, 1 = view
//         int32  cachedCpLength; // -1 = not yet computed
//     }
//
// These tests intentionally avoid string literals (those still emit i8*
// during Phase 2b-α — the literal-codegen flip is Phase 2b-β).
//
// All tests pin the *field layout* of the class — if any field is
// renamed/reordered/dropped, these break loudly.

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"

#include <cstdint>

using cajeta_test::CajetaJit;

namespace {
int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}
} // namespace

// String can be instantiated as a heap class. Today `String` is a
// primitive alias for i8* and cannot be `heap`-allocated — this test
// fails until cajeta.lang.String is a real class.
TEST(StringClassTests, canHeapInstantiate) {
    auto src =
        "package test;\n"
        "import cajeta.lang.String;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        String s = heap String();\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 0);
}

// String.byteLength field is reachable as int32 and round-trips a write.
TEST(StringClassTests, byteLengthFieldRoundTrips) {
    auto src =
        "package test;\n"
        "import cajeta.lang.String;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        String s = heap String();\n"
        "        s.byteLength = 12345;\n"
        "        return s.byteLength;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 12345);
}

// String.mode is the owned/view discriminator: 0 = owned, 1 = view.
TEST(StringClassTests, modeFieldOwnedVsView) {
    auto src =
        "package test;\n"
        "import cajeta.lang.String;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        String s = heap String();\n"
        "        s.mode = 1;\n"
        "        return s.mode;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// String.cachedCpLength is the lazy codepoint-count cache; -1 = not yet
// computed. Pin that the field exists and round-trips a sentinel.
TEST(StringClassTests, cachedCpLengthRoundTrips) {
    auto src =
        "package test;\n"
        "import cajeta.lang.String;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        String s = heap String();\n"
        "        s.cachedCpLength = -1;\n"
        "        return s.cachedCpLength;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), -1);
}

// The four fields are independently addressable — write all, read all
// back, fold into a single int32. Pins that no field overlap or
// reordering broke layout.
TEST(StringClassTests, allFieldsIndependentlyAddressable) {
    auto src =
        "package test;\n"
        "import cajeta.lang.String;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        String s = heap String();\n"
        "        s.byteLength = 7;\n"
        "        s.mode = 0;\n"
        "        s.cachedCpLength = 5;\n"
        "        return s.byteLength * 1000\n"
        "             + s.mode * 100\n"
        "             + s.cachedCpLength;\n"
        "    }\n"
        "}\n";
    // 7*1000 + 0*100 + 5 = 7005
    EXPECT_EQ(runI32(src), 7005);
}
