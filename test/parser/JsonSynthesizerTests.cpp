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
int64_t runI64(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int64_t (*)()>("run");
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

// Parse `{"n":1234567890123}` into an int64 field.
// Bytes:  { " n " : 1 2 3 4 5 6 7 8 9 0 1 2 3 }
TEST(JsonSynthesizerTests, parseInt64Field) {
    auto src =
        "package test;\n"
        "import cajeta.codec.json.Json;\n"
        "public class Box {\n"
        "    public int64 n;\n"
        "}\n"
        "public final class D {\n"
        "    public static int64 run() {\n"
        "        int8[] buf = new int8[19];\n"
        "        buf[0] = (int8) 0x7B;\n"   // '{'
        "        buf[1] = (int8) 0x22;\n"   // '"'
        "        buf[2] = (int8) 0x6E;\n"   // 'n'
        "        buf[3] = (int8) 0x22;\n"   // '"'
        "        buf[4] = (int8) 0x3A;\n"   // ':'
        "        buf[5] = (int8) 0x31;\n"   // '1'
        "        buf[6] = (int8) 0x32;\n"   // '2'
        "        buf[7] = (int8) 0x33;\n"   // '3'
        "        buf[8] = (int8) 0x34;\n"   // '4'
        "        buf[9] = (int8) 0x35;\n"   // '5'
        "        buf[10] = (int8) 0x36;\n"  // '6'
        "        buf[11] = (int8) 0x37;\n"  // '7'
        "        buf[12] = (int8) 0x38;\n"  // '8'
        "        buf[13] = (int8) 0x39;\n"  // '9'
        "        buf[14] = (int8) 0x30;\n"  // '0'
        "        buf[15] = (int8) 0x31;\n"  // '1'
        "        buf[16] = (int8) 0x32;\n"  // '2'
        "        buf[17] = (int8) 0x33;\n"  // '3'
        "        buf[18] = (int8) 0x7D;\n"  // '}'
        "        Box b = Json.parseT<Box>(buf, (int64) 19);\n"
        "        return b.n;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI64(src), (int64_t) 1234567890123LL);
}

// Parse `{"flag":true}` into a boolean field.
TEST(JsonSynthesizerTests, parseBooleanTrueField) {
    auto src =
        "package test;\n"
        "import cajeta.codec.json.Json;\n"
        "public class Box {\n"
        "    public boolean flag;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        // {"flag":true}
        "        int8[] buf = new int8[13];\n"
        "        buf[0] = (int8) 0x7B;\n"   // '{'
        "        buf[1] = (int8) 0x22;\n"   // '"'
        "        buf[2] = (int8) 0x66;\n"   // 'f'
        "        buf[3] = (int8) 0x6C;\n"   // 'l'
        "        buf[4] = (int8) 0x61;\n"   // 'a'
        "        buf[5] = (int8) 0x67;\n"   // 'g'
        "        buf[6] = (int8) 0x22;\n"   // '"'
        "        buf[7] = (int8) 0x3A;\n"   // ':'
        "        buf[8] = (int8) 0x74;\n"   // 't'
        "        buf[9] = (int8) 0x72;\n"   // 'r'
        "        buf[10] = (int8) 0x75;\n"  // 'u'
        "        buf[11] = (int8) 0x65;\n"  // 'e'
        "        buf[12] = (int8) 0x7D;\n"  // '}'
        "        Box b = Json.parseT<Box>(buf, (int64) 13);\n"
        "        if (b.flag) return 1;\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// Parse `{"flag":false}` into a boolean field.
TEST(JsonSynthesizerTests, parseBooleanFalseField) {
    auto src =
        "package test;\n"
        "import cajeta.codec.json.Json;\n"
        "public class Box {\n"
        "    public boolean flag;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        // {"flag":false}
        "        int8[] buf = new int8[14];\n"
        "        buf[0] = (int8) 0x7B;\n"
        "        buf[1] = (int8) 0x22;\n"
        "        buf[2] = (int8) 0x66;\n"   // 'f'
        "        buf[3] = (int8) 0x6C;\n"   // 'l'
        "        buf[4] = (int8) 0x61;\n"   // 'a'
        "        buf[5] = (int8) 0x67;\n"   // 'g'
        "        buf[6] = (int8) 0x22;\n"
        "        buf[7] = (int8) 0x3A;\n"
        "        buf[8] = (int8) 0x66;\n"   // 'f'
        "        buf[9] = (int8) 0x61;\n"   // 'a'
        "        buf[10] = (int8) 0x6C;\n"  // 'l'
        "        buf[11] = (int8) 0x73;\n"  // 's'
        "        buf[12] = (int8) 0x65;\n"  // 'e'
        "        buf[13] = (int8) 0x7D;\n"
        "        Box b = Json.parseT<Box>(buf, (int64) 14);\n"
        "        if (b.flag) return 1;\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 0);
}

// Parse `{"name":"hi"}` into a class with one String field. The
// synthesizer materializes a view-mode String from the JSON token's
// inner bytes (no quotes), via the (int8[], int32) String ctor.
TEST(JsonSynthesizerTests, parseStringField) {
    auto src =
        "package test;\n"
        "import cajeta.codec.json.Json;\n"
        "import cajeta.lang.String;\n"
        "public class Person {\n"
        "    public String name;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        // {"name":"hi"} → 13 bytes
        "        int8[] buf = new int8[13];\n"
        "        buf[0]  = (int8) 0x7B;\n"   // '{'
        "        buf[1]  = (int8) 0x22;\n"   // '"'
        "        buf[2]  = (int8) 0x6E;\n"   // 'n'
        "        buf[3]  = (int8) 0x61;\n"   // 'a'
        "        buf[4]  = (int8) 0x6D;\n"   // 'm'
        "        buf[5]  = (int8) 0x65;\n"   // 'e'
        "        buf[6]  = (int8) 0x22;\n"   // '"'
        "        buf[7]  = (int8) 0x3A;\n"   // ':'
        "        buf[8]  = (int8) 0x22;\n"   // '"'
        "        buf[9]  = (int8) 0x68;\n"   // 'h'
        "        buf[10] = (int8) 0x69;\n"   // 'i'
        "        buf[11] = (int8) 0x22;\n"   // '"'
        "        buf[12] = (int8) 0x7D;\n"   // '}'
        "        Person p = Json.parseT<Person>(buf, (int64) 13);\n"
        // p.name should be a 2-codepoint String ("hi")
        "        return (int32) p.name.count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 2);
}

// Mixed: parse `{"id":7,"n":99,"flag":true}` into a class with all 3
// supported field types. Pins multi-field key dispatch.
TEST(JsonSynthesizerTests, parseMixedInt32Int64Boolean) {
    auto src =
        "package test;\n"
        "import cajeta.codec.json.Json;\n"
        "public class Mix {\n"
        "    public int32 id;\n"
        "    public int64 n;\n"
        "    public boolean flag;\n"
        "}\n"
        "public final class D {\n"
        "    public static int64 run() {\n"
        // {"id":7,"n":99,"flag":true} — 26 bytes
        "        int8[] buf = new int8[26];\n"
        "        buf[0]  = (int8) 0x7B;\n"  // '{'
        "        buf[1]  = (int8) 0x22;\n"  // '"'
        "        buf[2]  = (int8) 0x69;\n"  // 'i'
        "        buf[3]  = (int8) 0x64;\n"  // 'd'
        "        buf[4]  = (int8) 0x22;\n"  // '"'
        "        buf[5]  = (int8) 0x3A;\n"  // ':'
        "        buf[6]  = (int8) 0x37;\n"  // '7'
        "        buf[7]  = (int8) 0x2C;\n"  // ','
        "        buf[8]  = (int8) 0x22;\n"  // '"'
        "        buf[9]  = (int8) 0x6E;\n"  // 'n'
        "        buf[10] = (int8) 0x22;\n"  // '"'
        "        buf[11] = (int8) 0x3A;\n"  // ':'
        "        buf[12] = (int8) 0x39;\n"  // '9'
        "        buf[13] = (int8) 0x39;\n"  // '9'
        "        buf[14] = (int8) 0x2C;\n"  // ','
        "        buf[15] = (int8) 0x22;\n"  // '"'
        "        buf[16] = (int8) 0x66;\n"  // 'f'
        "        buf[17] = (int8) 0x6C;\n"  // 'l'
        "        buf[18] = (int8) 0x61;\n"  // 'a'
        "        buf[19] = (int8) 0x67;\n"  // 'g'
        "        buf[20] = (int8) 0x22;\n"  // '"'
        "        buf[21] = (int8) 0x3A;\n"  // ':'
        "        buf[22] = (int8) 0x74;\n"  // 't'
        "        buf[23] = (int8) 0x72;\n"  // 'r'
        "        buf[24] = (int8) 0x75;\n"  // 'u'
        "        buf[25] = (int8) 0x65;\n"  // 'e' -- }
        // Actually we need '}' but we used 26 bytes — fix the length:
        // {"id":7,"n":99,"flag":true} is 27 bytes. Adjust:
        "        int8[] buf2 = new int8[27];\n"
        "        int32 i = 0;\n"
        "        while (i < 26) { buf2[i] = buf[i]; i = i + 1; }\n"
        "        buf2[26] = (int8) 0x7D;\n"  // '}'
        "        Mix m = Json.parseT<Mix>(buf2, (int64) 27);\n"
        "        int64 r = (int64) m.id * 1000 + m.n;\n"  // 7*1000 + 99 = 7099
        "        if (m.flag) r = r + 1000000;\n"
        "        return r;\n"
        "    }\n"
        "}\n";
    // 7099 + 1000000 (flag) = 1007099
    EXPECT_EQ(runI64(src), (int64_t) 1007099);
}
