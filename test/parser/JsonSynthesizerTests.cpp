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
double runF64(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<double (*)()>("run");
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
        "        Box b = Json.parse<Box>(buf, (int64) 9);\n"
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
        "        Box b = Json.parse<Box>(buf, (int64) 19);\n"
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
        "        Box b = Json.parse<Box>(buf, (int64) 13);\n"
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
        "        Box b = Json.parse<Box>(buf, (int64) 14);\n"
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
        "        Person p = Json.parse<Person>(buf, (int64) 13);\n"
        // p.name should be a 2-codepoint String ("hi")
        "        return (int32) p.name.count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 2);
}

// Parse `{"point":{"x":7}}` into Outer { Inner point; } with
// Inner { int32 x; }. Pins synthesizer's recursive nested-class
// dispatch — Json.parseObjectFromReaderT<Inner> is called from the
// parent's field arm with the shared JsonReader.
TEST(JsonSynthesizerTests, parseNestedClass) {
    auto src =
        "package test;\n"
        "import cajeta.codec.json.Json;\n"
        "public class Inner { public int32 x; }\n"
        "public class Outer { public Inner point; }\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        // {"point":{"x":7}} → 17 bytes
        "        int8[] buf = new int8[17];\n"
        "        buf[0]  = (int8) 0x7B;\n"  // '{'
        "        buf[1]  = (int8) 0x22;\n"  // '"'
        "        buf[2]  = (int8) 0x70;\n"  // 'p'
        "        buf[3]  = (int8) 0x6F;\n"  // 'o'
        "        buf[4]  = (int8) 0x69;\n"  // 'i'
        "        buf[5]  = (int8) 0x6E;\n"  // 'n'
        "        buf[6]  = (int8) 0x74;\n"  // 't'
        "        buf[7]  = (int8) 0x22;\n"  // '"'
        "        buf[8]  = (int8) 0x3A;\n"  // ':'
        "        buf[9]  = (int8) 0x7B;\n"  // '{'
        "        buf[10] = (int8) 0x22;\n"  // '"'
        "        buf[11] = (int8) 0x78;\n"  // 'x'
        "        buf[12] = (int8) 0x22;\n"  // '"'
        "        buf[13] = (int8) 0x3A;\n"  // ':'
        "        buf[14] = (int8) 0x37;\n"  // '7'
        "        buf[15] = (int8) 0x7D;\n"  // '}'
        "        buf[16] = (int8) 0x7D;\n"  // '}'
        "        Outer o = Json.parse<Outer>(buf, (int64) 17);\n"
        "        return o.point.x;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

// Round-trip nested class through toBytes<T> + parse<T>.
TEST(JsonSynthesizerTests, roundTripNestedClass) {
    auto src =
        "package test;\n"
        "import cajeta.codec.json.Json;\n"
        "public class Inner { public int32 x; }\n"
        "public class Outer { public Inner point; }\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Outer a = heap Outer();\n"
        "        Inner ip = heap Inner();\n"
        "        ip.x = 99;\n"
        "        a.point = ip;\n"
        "        int8[] bytes = Json.toBytes<Outer>(a);\n"
        "        int64 n = (int64) bytes.count();\n"
        "        Outer b = Json.parse<Outer>(bytes, n);\n"
        "        return b.point.x;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 99);
}

// Parse `{"x":3.14}` into a float64 field.
TEST(JsonSynthesizerTests, parseFloat64Field) {
    auto src =
        "package test;\n"
        "import cajeta.codec.json.Json;\n"
        "public class Box {\n"
        "    public float64 x;\n"
        "}\n"
        "public final class D {\n"
        "    public static float64 run() {\n"
        // {"x":3.14} = 10 bytes
        "        int8[] buf = new int8[10];\n"
        "        buf[0] = (int8) 0x7B;\n"   // '{'
        "        buf[1] = (int8) 0x22;\n"   // '"'
        "        buf[2] = (int8) 0x78;\n"   // 'x'
        "        buf[3] = (int8) 0x22;\n"   // '"'
        "        buf[4] = (int8) 0x3A;\n"   // ':'
        "        buf[5] = (int8) 0x33;\n"   // '3'
        "        buf[6] = (int8) 0x2E;\n"   // '.'
        "        buf[7] = (int8) 0x31;\n"   // '1'
        "        buf[8] = (int8) 0x34;\n"   // '4'
        "        buf[9] = (int8) 0x7D;\n"   // '}'
        "        Box b = Json.parse<Box>(buf, (int64) 10);\n"
        "        return b.x;\n"
        "    }\n"
        "}\n";
    EXPECT_NEAR(runF64(src), 3.14, 1e-6);
}

// Round-trip float64 — write, re-parse, compare.
TEST(JsonSynthesizerTests, roundTripFloat64) {
    auto src =
        "package test;\n"
        "import cajeta.codec.json.Json;\n"
        "public class Box {\n"
        "    public float64 x;\n"
        "}\n"
        "public final class D {\n"
        "    public static float64 run() {\n"
        "        Box a = heap Box();\n"
        "        a.x = 2.5;\n"
        "        int8[] bytes = Json.toBytes<Box>(a);\n"
        "        int64 n = (int64) bytes.count();\n"
        "        Box b = Json.parse<Box>(bytes, n);\n"
        "        return b.x;\n"
        "    }\n"
        "}\n";
    EXPECT_NEAR(runF64(src), 2.5, 1e-9);
}

// Round-trip int32 via Json.toBytes<Box> → Json.parse<Box>.
TEST(JsonSynthesizerTests, roundTripInt32) {
    auto src =
        "package test;\n"
        "import cajeta.codec.json.Json;\n"
        "public class Box {\n"
        "    public int32 id;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Box a = heap Box();\n"
        "        a.id = 42;\n"
        "        int8[] bytes = Json.toBytes<Box>(a);\n"
        "        int64 n = (int64) bytes.count();\n"
        "        Box b = Json.parse<Box>(bytes, n);\n"
        "        return b.id;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// Round-trip int32 + int64 + boolean together.
TEST(JsonSynthesizerTests, roundTripMixedPrimitives) {
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
        "        Mix a = heap Mix();\n"
        "        a.id = 7;\n"
        "        a.n = (int64) 99999999999;\n"
        "        a.flag = true;\n"
        "        int8[] bytes = Json.toBytes<Mix>(a);\n"
        "        int64 len = (int64) bytes.count();\n"
        "        Mix b = Json.parse<Mix>(bytes, len);\n"
        "        int64 r = b.n;\n"
        "        if (b.flag) r = r + 1;\n"
        "        return r + (int64) (b.id * 1000000);\n"
        "    }\n"
        "}\n";
    // 99999999999 + 1 + 7*1000000 = 100007000000
    EXPECT_EQ(runI64(src), (int64_t) 100007000000LL);
}

// ---- Phase 4b commit 7: array fields ----

// Parse `{"ids":[1,2,3]}` into a class with one int32[] field.
// Pins the inner-array loop the synthesizer must emit: consume
// START_ARRAY, collect NUMBER tokens into a tmp ArrayList, copy
// into a sized int32[] on END_ARRAY.
TEST(JsonSynthesizerTests, parseInt32ArrayField) {
    auto src =
        "package test;\n"
        "import cajeta.codec.json.Json;\n"
        "public class Bag {\n"
        "    public int32[] ids;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        // {"ids":[1,2,3]} → 15 bytes
        "        int8[] buf = new int8[15];\n"
        "        buf[0]  = (int8) 0x7B;\n"  // '{'
        "        buf[1]  = (int8) 0x22;\n"  // '"'
        "        buf[2]  = (int8) 0x69;\n"  // 'i'
        "        buf[3]  = (int8) 0x64;\n"  // 'd'
        "        buf[4]  = (int8) 0x73;\n"  // 's'
        "        buf[5]  = (int8) 0x22;\n"  // '"'
        "        buf[6]  = (int8) 0x3A;\n"  // ':'
        "        buf[7]  = (int8) 0x5B;\n"  // '['
        "        buf[8]  = (int8) 0x31;\n"  // '1'
        "        buf[9]  = (int8) 0x2C;\n"  // ','
        "        buf[10] = (int8) 0x32;\n"  // '2'
        "        buf[11] = (int8) 0x2C;\n"  // ','
        "        buf[12] = (int8) 0x33;\n"  // '3'
        "        buf[13] = (int8) 0x5D;\n"  // ']'
        "        buf[14] = (int8) 0x7D;\n"  // '}'
        "        Bag b = Json.parse<Bag>(buf, (int64) 15);\n"
        "        return b.ids[0] * 100 + b.ids[1] * 10 + b.ids[2];\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 123);
}

// Empty array `{"ids":[]}`: parser produces a zero-length int32[].
// Pins the START_ARRAY-immediately-followed-by-END_ARRAY case.
TEST(JsonSynthesizerTests, parseEmptyInt32ArrayField) {
    auto src =
        "package test;\n"
        "import cajeta.codec.json.Json;\n"
        "public class Bag {\n"
        "    public int32[] ids;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        // {"ids":[]} → 10 bytes
        "        int8[] buf = new int8[10];\n"
        "        buf[0] = (int8) 0x7B;\n"  // '{'
        "        buf[1] = (int8) 0x22;\n"  // '"'
        "        buf[2] = (int8) 0x69;\n"  // 'i'
        "        buf[3] = (int8) 0x64;\n"  // 'd'
        "        buf[4] = (int8) 0x73;\n"  // 's'
        "        buf[5] = (int8) 0x22;\n"  // '"'
        "        buf[6] = (int8) 0x3A;\n"  // ':'
        "        buf[7] = (int8) 0x5B;\n"  // '['
        "        buf[8] = (int8) 0x5D;\n"  // ']'
        "        buf[9] = (int8) 0x7D;\n"  // '}'
        "        Bag b = Json.parse<Bag>(buf, (int64) 10);\n"
        "        return (int32) b.ids.count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 0);
}

// Round-trip int32[] through Json.toBytes<Bag> → Json.parse<Bag>.
TEST(JsonSynthesizerTests, roundTripInt32Array) {
    auto src =
        "package test;\n"
        "import cajeta.codec.json.Json;\n"
        "public class Bag {\n"
        "    public int32[] ids;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Bag a = heap Bag();\n"
        "        a.ids = new int32[3];\n"
        "        a.ids[0] = 4;\n"
        "        a.ids[1] = 5;\n"
        "        a.ids[2] = 6;\n"
        "        int8[] bytes = Json.toBytes<Bag>(a);\n"
        "        int64 len = (int64) bytes.count();\n"
        "        Bag b = Json.parse<Bag>(bytes, len);\n"
        "        return b.ids[0] * 100 + b.ids[1] * 10 + b.ids[2];\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 456);
}

// Round-trip an int64[] through both write and read paths.
TEST(JsonSynthesizerTests, roundTripInt64Array) {
    auto src =
        "package test;\n"
        "import cajeta.codec.json.Json;\n"
        "public class Bag {\n"
        "    public int64[] ns;\n"
        "}\n"
        "public final class D {\n"
        "    public static int64 run() {\n"
        "        Bag a = heap Bag();\n"
        "        a.ns = new int64[2];\n"
        "        a.ns[0] = (int64) 100000000000;\n"
        "        a.ns[1] = (int64) 200000000000;\n"
        "        int8[] bytes = Json.toBytes<Bag>(a);\n"
        "        int64 len = (int64) bytes.count();\n"
        "        Bag b = Json.parse<Bag>(bytes, len);\n"
        "        return b.ns[0] + b.ns[1];\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI64(src), (int64_t) 300000000000LL);
}

// Round-trip a boolean[] through both paths.
TEST(JsonSynthesizerTests, roundTripBooleanArray) {
    auto src =
        "package test;\n"
        "import cajeta.codec.json.Json;\n"
        "public class Bag {\n"
        "    public boolean[] flags;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Bag a = heap Bag();\n"
        "        a.flags = new boolean[3];\n"
        "        a.flags[0] = true;\n"
        "        a.flags[1] = false;\n"
        "        a.flags[2] = true;\n"
        "        int8[] bytes = Json.toBytes<Bag>(a);\n"
        "        int64 len = (int64) bytes.count();\n"
        "        Bag b = Json.parse<Bag>(bytes, len);\n"
        "        int32 r = 0;\n"
        "        if (b.flags[0]) r = r + 4;\n"
        "        if (b.flags[1]) r = r + 2;\n"
        "        if (b.flags[2]) r = r + 1;\n"
        "        return r;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 5);  // 4 + 0 + 1
}

// Round-trip a float64[] through both paths.
TEST(JsonSynthesizerTests, roundTripFloat64Array) {
    auto src =
        "package test;\n"
        "import cajeta.codec.json.Json;\n"
        "public class Bag {\n"
        "    public float64[] xs;\n"
        "}\n"
        "public final class D {\n"
        "    public static float64 run() {\n"
        "        Bag a = heap Bag();\n"
        "        a.xs = new float64[2];\n"
        "        a.xs[0] = 1.5;\n"
        "        a.xs[1] = 2.25;\n"
        "        int8[] bytes = Json.toBytes<Bag>(a);\n"
        "        int64 len = (int64) bytes.count();\n"
        "        Bag b = Json.parse<Bag>(bytes, len);\n"
        "        return b.xs[0] + b.xs[1];\n"
        "    }\n"
        "}\n";
    EXPECT_NEAR(runF64(src), 3.75, 1e-9);
}

// Mixed primitives + array — multi-field dispatch with an array
// in the mix. Pins that array consumption doesn't desynchronize
// the outer key loop.
TEST(JsonSynthesizerTests, roundTripMixedWithArray) {
    auto src =
        "package test;\n"
        "import cajeta.codec.json.Json;\n"
        "public class Mix {\n"
        "    public int32 id;\n"
        "    public int32[] ids;\n"
        "    public boolean flag;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Mix a = heap Mix();\n"
        "        a.id = 9;\n"
        "        a.ids = new int32[2];\n"
        "        a.ids[0] = 7;\n"
        "        a.ids[1] = 8;\n"
        "        a.flag = true;\n"
        "        int8[] bytes = Json.toBytes<Mix>(a);\n"
        "        int64 len = (int64) bytes.count();\n"
        "        Mix m = Json.parse<Mix>(bytes, len);\n"
        "        int32 r = m.id * 100 + m.ids[0] * 10 + m.ids[1];\n"
        "        if (m.flag) r = r + 1000;\n"
        "        return r;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1978);  // 1000 + 900 + 70 + 8
}

// `Json.parse<T>(String)` convenience overload. Hands the String's
// `bytes` + `byteLength` to the byte-buffer variant in one step —
// the synthesizer must NOT overwrite this overload's delegation
// body with the byte-walking body even though the name matches.
TEST(JsonSynthesizerTests, parseFromStringOverload) {
    auto src =
        "package test;\n"
        "import cajeta.codec.json.Json;\n"
        "public class Box {\n"
        "    public int32 id;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        String s = \"{\\\"id\\\":99}\";\n"
        "        Box b = Json.parse<Box>(s);\n"
        "        return b.id;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 99);
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
        "        Mix m = Json.parse<Mix>(buf2, (int64) 27);\n"
        "        int64 r = (int64) m.id * 1000 + m.n;\n"  // 7*1000 + 99 = 7099
        "        if (m.flag) r = r + 1000000;\n"
        "        return r;\n"
        "    }\n"
        "}\n";
    // 7099 + 1000000 (flag) = 1007099
    EXPECT_EQ(runI64(src), (int64_t) 1007099);
}
