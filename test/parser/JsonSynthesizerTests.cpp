// Phase 4b — Tier 1 JSON codegen synthesizer.
//
// `Json.parse<T>(bytes, length)` and `Json.toBytes<T>(value)` are
// method-level templates whose bodies are SYNTHESIZED per-T at
// instantiation time. The compiler walks T's declared fields and
// emits a per-field reader/writer chain — no JsonValue tree, no
// runtime reflection.
//
// Commit 1 scope: a single int32 field, no annotations, no
// unknown-key handling (we don't put unknown keys in the test JSON).
// Follow-up commits add int64/boolean/float/String, then nested
// classes, then annotation surface.
//
// See cajeta-docs/stdlib/codec/Json.md § Tier 1.

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

// Parse `{"id":42}` into a Box class with one int32 field, return its id.
// The synthesized parse body is the smallest possible:
//   - consume START_OBJECT
//   - one iteration: KEY "id", then NUMBER 42 → out.id = 42
//   - consume END_OBJECT
TEST(JsonSynthesizerTests, parseSingleInt32Field) {
    auto src =
        "package test;\n"
        "import cajeta.codec.json.Json;\n"
        "public class Box {\n"
        "    public int32 id;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        // JSON bytes for `{"id":42}` — 10 bytes total
        "        int8[] buf = new int8[10];\n"
        "        buf[0] = (int8) 0x7B;\n"   // '{'
        "        buf[1] = (int8) 0x22;\n"   // '"'
        "        buf[2] = (int8) 0x69;\n"   // 'i'
        "        buf[3] = (int8) 0x64;\n"   // 'd'
        "        buf[4] = (int8) 0x22;\n"   // '"'
        "        buf[5] = (int8) 0x3A;\n"   // ':'
        "        buf[6] = (int8) 0x34;\n"   // '4'
        "        buf[7] = (int8) 0x32;\n"   // '2'
        "        buf[8] = (int8) 0x7D;\n"   // '}'
        "        buf[9] = (int8) 0x00;\n"   // (unused; length=9 passed)
        "        Box b = Json.parseT<Box>(buf, (int64) 9);\n"
        "        return b.id;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}
