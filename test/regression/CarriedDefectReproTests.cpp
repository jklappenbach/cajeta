//
// stdlib-ownership-convention U10.3 — the two defects parked on the focus
// stack as [explore:] lines, promoted to tested plan items so they stop
// living as free text.
//
// Both tests assert the CORRECT behavior: RED while the defect stands,
// green with the fix, unedited. MEASURED 2026-08-19: both still fail, so
// both are DISABLED_ — a documented defect must not poison the gate.
// Enable with each fix; they should go green with no edit.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn ? fn() : -1;
}

}  // namespace

// 10.3.1 — JsonWriter must escape control bytes (RFC 8259: U+0000..U+001F
// MUST be \uXXXX-escaped; today only `"` and `\` are). The cell writes a
// string containing 0x01 and 0x1F and scans the emitted JSON for any raw
// byte < 0x20 outside the structural whitespace set — one found means the
// writer emitted invalid JSON (due with the llama tokenizer work,
// cajeta-llama 13.7/13.8).
TEST(CarriedDefectReproTests, DISABLED_jsonWriterEscapesControlBytes) {
    std::string src =
        "package test;\n"
        "import cajeta.codec.json.JsonWriter;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int8[] payload #= heap int8[3];\n"
        "        payload[0] = (int8) 65;\n"    // 'A'
        "        payload[1] = (int8) 1;\n"     // control byte
        "        payload[2] = (int8) 31;\n"    // control byte
        "        JsonWriter w = heap JsonWriter();\n"
        "        w.writeString(payload, 3);\n"
        "        int8[] out #= w.toBytes();\n"
        "        int32 i = 0;\n"
        "        while (i < out.count()) {\n"
        "            int32 b = ((int32) out[i]) & 255;\n"
        "            if (b < 32 && b != 9 && b != 10 && b != 13) {\n"
        "                return -2;\n"          // raw control byte escaped nothing
        "            }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(1, runI32(src));
}

// 10.3.2 — a local shadowing a same-named field of a DIFFERENT type must
// resolve member lookup against the LOCAL's type (cajeta-llama U13; kin to
// the block-scope shadow that shipped corrupt IR — blocks share the method
// Scope map). `count` is an int32 field; the local `count` is a String, and
// `count.count()` must be String.count().
TEST(CarriedDefectReproTests, DISABLED_localShadowingFieldResolvesMembersOnLocal) {
    std::string src =
        "package test;\n"
        "public final class D {\n"
        "    int32 count;\n"
        "    public D() { this.count = 999; }\n"
        "    public int32 go() {\n"
        "        String count = \"abc\";\n"
        "        return (int32) count.count();\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        D d = heap D();\n"
        "        return d.go();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(3, runI32(src));
}
