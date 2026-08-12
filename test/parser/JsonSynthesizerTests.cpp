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
// See docs/specification/codec/json/Json.md § Tier 1.

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
        "        int8[] buf = heap int8[10];\n"
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
        "        int8[] buf = heap int8[19];\n"
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
        "        int8[] buf = heap int8[13];\n"
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
        "        int8[] buf = heap int8[14];\n"
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
        "        int8[] buf = heap int8[13];\n"
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
        "        int8[] buf = heap int8[17];\n"
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
        "        int8[] buf = heap int8[10];\n"
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
        "        int8[] buf = heap int8[15];\n"
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
        "        int8[] buf = heap int8[10];\n"
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
        "        a.ids = heap int32[3];\n"
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
        "        a.ns = heap int64[2];\n"
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
        "        a.flags = heap boolean[3];\n"
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
        "        a.xs = heap float64[2];\n"
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
        "        a.ids = heap int32[2];\n"
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

// ---- Phase 4b commit 8: @JsonProperty + @JsonIgnore annotations ----

// `@JsonProperty("custom_name")` renames the field's wire key on both
// write and read sides. The field's source-declared name stays
// available for Cajeta-level access; only the JSON key changes.
TEST(JsonSynthesizerTests, jsonPropertyRenameOnWrite) {
    auto src =
        "package test;\n"
        "import cajeta.codec.json.Json;\n"
        "public class Box {\n"
        "    @JsonProperty(\"user_id\")\n"
        "    public int32 id;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Box a = heap Box();\n"
        "        a.id = 7;\n"
        "        int8[] bytes = Json.toBytes<Box>(a);\n"
        // Expect: {"user_id":7} → 13 bytes (1 + 9 quoted-key + 1 + 1 + 1)
        "        return (int32) bytes.count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 13);
}

// Read side — JSON has the renamed key, struct's declared name is
// different. Synthesizer must look for the annotation-provided
// key, not the declared name.
TEST(JsonSynthesizerTests, jsonPropertyRenameOnRead) {
    auto src =
        "package test;\n"
        "import cajeta.codec.json.Json;\n"
        "public class Box {\n"
        "    @JsonProperty(\"user_id\")\n"
        "    public int32 id;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        // {"user_id":42} → 15 bytes
        "        String s = \"{\\\"user_id\\\":42}\";\n"
        "        Box b = Json.parse<Box>(s);\n"
        "        return b.id;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// Round-trip with rename. Pins both directions agree.
TEST(JsonSynthesizerTests, jsonPropertyRoundTrip) {
    auto src =
        "package test;\n"
        "import cajeta.codec.json.Json;\n"
        "public class Box {\n"
        "    @JsonProperty(\"user_id\")\n"
        "    public int32 id;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Box a = heap Box();\n"
        "        a.id = 13;\n"
        "        int8[] bytes = Json.toBytes<Box>(a);\n"
        "        int64 n = (int64) bytes.count();\n"
        "        Box b = Json.parse<Box>(bytes, n);\n"
        "        return b.id;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 13);
}

// `@JsonIgnore` on a field skips it entirely — neither written nor
// consumed on read. Wire output for a one-field class with the
// field annotated is `{}` (2 bytes). Reading `{"secret":99}` into
// the same class doesn't populate the field (stays at default 0).
TEST(JsonSynthesizerTests, jsonIgnoreNotWritten) {
    auto src =
        "package test;\n"
        "import cajeta.codec.json.Json;\n"
        "public class Box {\n"
        "    @JsonIgnore\n"
        "    public int32 secret;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Box a = heap Box();\n"
        "        a.secret = 999;\n"
        "        int8[] bytes = Json.toBytes<Box>(a);\n"
        // Expect: {} → 2 bytes
        "        return (int32) bytes.count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 2);
}

// `@JsonIgnore` on read — JSON key "secret" appears in input but
// must NOT populate the field. Field stays at its default value.
// The unknown-key arm should still consume the value cleanly.
TEST(JsonSynthesizerTests, jsonIgnoreNotRead) {
    auto src =
        "package test;\n"
        "import cajeta.codec.json.Json;\n"
        "public class Box {\n"
        "    @JsonIgnore\n"
        "    public int32 secret;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        // {"secret":42} — 14 bytes
        "        String s = \"{\\\"secret\\\":42}\";\n"
        "        Box b = Json.parse<Box>(s);\n"
        "        return b.secret;\n"   // default-zero, not 42
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 0);
}

// Asymmetric @JsonIgnore(onWrite=true) — read still happens, write
// omits. Field-set-from-input-then-never-echoed audit pattern.
TEST(JsonSynthesizerTests, jsonIgnoreOnWriteReadStillRuns) {
    auto src =
        "package test;\n"
        "import cajeta.codec.json.Json;\n"
        "public class AuditEvent {\n"
        "    public int32 id;\n"
        "    @JsonIgnore(onWrite = true)\n"
        "    public int32 internalTag;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        // Read: internalTag should still pick up 99.
        "        String s = \"{\\\"id\\\":7,\\\"internalTag\\\":99}\";\n"
        "        AuditEvent e = Json.parse<AuditEvent>(s);\n"
        "        if (e.internalTag != 99) return 0;\n"
        "        if (e.id != 7) return 0;\n"
        // Write: internalTag omitted.
        "        int8[] bytes = Json.toBytes<AuditEvent>(e);\n"
        // {"id":7} = 8 bytes.
        "        return (int32) bytes.count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 8);
}

// Asymmetric @JsonIgnore(onRead=true) — parser skips the key,
// writer still emits. The "computed/derived field" pattern: it
// exists on the object, is written out, but never accepted from
// external input.
TEST(JsonSynthesizerTests, jsonIgnoreOnReadWriteStillRuns) {
    auto src =
        "package test;\n"
        "import cajeta.codec.json.Json;\n"
        "public class Computed {\n"
        "    public int32 id;\n"
        "    @JsonIgnore(onRead = true)\n"
        "    public int32 derived;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        // Read: derived stays at default 0 (input value ignored).
        "        String s = \"{\\\"id\\\":3,\\\"derived\\\":777}\";\n"
        "        Computed c = Json.parse<Computed>(s);\n"
        "        if (c.derived != 0) return 0;\n"
        "        if (c.id != 3) return 0;\n"
        // Write: derived emits — but it's 0 right now.
        "        c.derived = 42;\n"
        "        int8[] bytes = Json.toBytes<Computed>(c);\n"
        // {"id":3,"derived":42} = 21 bytes.
        "        return (int32) bytes.count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 21);
}

// Bare @JsonIgnore (no args) still skips both — back-compat with
// the simple form. Asymmetric semantics only kick in when args
// are supplied.
TEST(JsonSynthesizerTests, jsonIgnoreBareStillSkipsBoth) {
    auto src =
        "package test;\n"
        "import cajeta.codec.json.Json;\n"
        "public class Both {\n"
        "    public int32 id;\n"
        "    @JsonIgnore\n"
        "    public int32 hidden;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        String s = \"{\\\"id\\\":1,\\\"hidden\\\":555}\";\n"
        "        Both b = Json.parse<Both>(s);\n"
        "        if (b.hidden != 0) return -1;\n"  // read-skipped → 0
        "        b.hidden = 999;\n"
        "        int8[] bytes = Json.toBytes<Both>(b);\n"
        // {"id":1} = 8 bytes; hidden write-skipped too.
        "        return (int32) bytes.count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 8);
}

// Probe: plain Optional<String> works outside the synthesizer.
TEST(JsonSynthesizerTests, probeOptionalStringDirect) {
    auto src =
        "package test;\n"
        "import cajeta.lang.Optional;\n"
        "import cajeta.lang.String;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        String s = \"alice\";\n"
        "        Optional<String> o = heap Optional<String>(true, s);\n"
        "        if (o == null) return -1;\n"
        "        if (o.isEmpty()) return -2;\n"
        "        String g = o.get();\n"
        "        return g.equals(\"alice\") ? 1 : 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// Probe: plain Optional<int32> works outside the synthesizer.
TEST(JsonSynthesizerTests, probeOptionalInt32Direct) {
    auto src =
        "package test;\n"
        "import cajeta.lang.Optional;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Optional<int32> o = heap Optional<int32>(true, 42);\n"
        "        if (o == null) return -1;\n"
        "        if (o.isEmpty()) return -2;\n"
        "        return o.get();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// ---- Optional<T> field type — present / absent / null distinction ----
//
// Three runtime states an Optional<T> field can carry, mapped to
// three wire states:
//
//   Field state                 Wire state
//   --------------              ----------
//   field == null               key absent from the object
//   field.isEmpty()             "key": null
//   field.isPresent()           "key": <value>

TEST(JsonSynthesizerTests, jsonOptionalInt32PresentReadsValue) {
    auto src =
        "package test;\n"
        "import cajeta.codec.json.Json;\n"
        "import cajeta.lang.Optional;\n"
        "public class WithOpt {\n"
        "    public Optional<int32> age;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        String s = \"{\\\"age\\\":42}\";\n"
        "        WithOpt w = Json.parse<WithOpt>(s);\n"
        "        if (w.age == null) return -1;\n"
        "        if (w.age.isEmpty()) return -2;\n"
        "        return w.age.get();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

TEST(JsonSynthesizerTests, jsonOptionalInt32NullValueProducesEmpty) {
    auto src =
        "package test;\n"
        "import cajeta.codec.json.Json;\n"
        "import cajeta.lang.Optional;\n"
        "public class WithOpt {\n"
        "    public Optional<int32> age;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        String s = \"{\\\"age\\\":null}\";\n"
        "        WithOpt w = Json.parse<WithOpt>(s);\n"
        "        if (w.age == null) return -1;\n"
        "        if (w.age.isEmpty()) return 1;\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

TEST(JsonSynthesizerTests, jsonOptionalKeyAbsentLeavesFieldNull) {
    auto src =
        "package test;\n"
        "import cajeta.codec.json.Json;\n"
        "import cajeta.lang.Optional;\n"
        "public class WithOpt {\n"
        "    public Optional<int32> age;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        String s = \"{}\";\n"
        "        WithOpt w = Json.parse<WithOpt>(s);\n"
        "        if (w.age == null) return 1;\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

TEST(JsonSynthesizerTests, jsonOptionalNullFieldOmittedFromWrite) {
    auto src =
        "package test;\n"
        "import cajeta.codec.json.Json;\n"
        "import cajeta.lang.Optional;\n"
        "public class WithOpt {\n"
        "    public int32 id;\n"
        "    public Optional<int32> age;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        WithOpt w = heap WithOpt();\n"
        "        w.id = 1;\n"
        "        // age stays null — should be omitted.\n"
        "        int8[] bytes = Json.toBytes<WithOpt>(w);\n"
        // {"id":1} = 8 bytes.
        "        return (int32) bytes.count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 8);
}

TEST(JsonSynthesizerTests, jsonOptionalEmptyWritesNull) {
    auto src =
        "package test;\n"
        "import cajeta.codec.json.Json;\n"
        "import cajeta.lang.Optional;\n"
        "public class WithOpt {\n"
        "    public Optional<int32> age;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        WithOpt w = heap WithOpt();\n"
        "        w.age = heap Optional<int32>(false, 0);\n"
        "        int8[] bytes = Json.toBytes<WithOpt>(w);\n"
        // {"age":null} = 12 bytes.
        "        return (int32) bytes.count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 12);
}

TEST(JsonSynthesizerTests, jsonOptionalStringPresentReadsValue) {
    auto src =
        "package test;\n"
        "import cajeta.codec.json.Json;\n"
        "import cajeta.lang.Optional;\n"
        "import cajeta.lang.String;\n"
        "public class WithOpt {\n"
        "    public Optional<String> name;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        String s = \"{\\\"name\\\":\\\"alice\\\"}\";\n"
        "        WithOpt w = Json.parse<WithOpt>(s);\n"
        "        if (w.name == null) return -1;\n"
        "        if (w.name.isEmpty()) return -2;\n"
        "        return 1;\n"  // just check presence, skip get() for now
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

TEST(JsonSynthesizerTests, jsonOptionalPresentWritesValue) {
    auto src =
        "package test;\n"
        "import cajeta.codec.json.Json;\n"
        "import cajeta.lang.Optional;\n"
        "public class WithOpt {\n"
        "    public Optional<int32> age;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        WithOpt w = heap WithOpt();\n"
        "        w.age = heap Optional<int32>(true, 99);\n"
        "        int8[] bytes = Json.toBytes<WithOpt>(w);\n"
        // {"age":99} = 10 bytes.
        "        return (int32) bytes.count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 10);
}

// ---- @JsonRaw — wire bytes pass through verbatim ------------------

// READ side: a @JsonRaw int8[] field captures the value's wire
// bytes including surrounding quotes for strings. The captured
// span should reconstruct as valid JSON.
TEST(JsonSynthesizerTests, jsonRawCapturesStringWireForm) {
    auto src =
        "package test;\n"
        "import cajeta.codec.json.Json;\n"
        "public class WithRaw {\n"
        "    @JsonRaw\n"
        "    public int8[] payload;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        // "payload":"hi" → captured bytes include the quotes, so 4 bytes.
        "        String s = \"{\\\"payload\\\":\\\"hi\\\"}\";\n"
        "        WithRaw w = Json.parse<WithRaw>(s);\n"
        "        return (int32) w.payload.count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 4);
}

// READ side: a number literal is NOT delimited, so the captured
// span is just the digit text.
TEST(JsonSynthesizerTests, jsonRawCapturesNumberWireForm) {
    auto src =
        "package test;\n"
        "import cajeta.codec.json.Json;\n"
        "public class WithRaw {\n"
        "    @JsonRaw\n"
        "    public int8[] amount;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        // "amount":42 → captured bytes are "42" → 2 bytes.
        "        String s = \"{\\\"amount\\\":42}\";\n"
        "        WithRaw w = Json.parse<WithRaw>(s);\n"
        "        return (int32) w.amount.count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 2);
}

// WRITE side: the bytes are emitted verbatim. Round-trip is
// byte-stable on primitives. Read in `"foo"` (4 bytes incl.
// quotes), write back out → wire output has the same quoted
// "foo".
TEST(JsonSynthesizerTests, jsonRawRoundTripsStringPrimitive) {
    auto src =
        "package test;\n"
        "import cajeta.codec.json.Json;\n"
        "public class WithRaw {\n"
        "    @JsonRaw\n"
        "    public int8[] payload;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        String s = \"{\\\"payload\\\":\\\"hi\\\"}\";\n"
        "        WithRaw w = Json.parse<WithRaw>(s);\n"
        "        int8[] out = Json.toBytes<WithRaw>(w);\n"
        // Round-trip: 16-byte output {"payload":"hi"} matches input.
        "        return (int32) out.count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 16);
}

// Mixed: one normal field, one renamed, one ignored. Pins that the
// annotation pass doesn't disturb un-annotated fields.
TEST(JsonSynthesizerTests, mixedAnnotatedFields) {
    auto src =
        "package test;\n"
        "import cajeta.codec.json.Json;\n"
        "public class Mix {\n"
        "    public int32 id;\n"
        "    @JsonProperty(\"display_name\")\n"
        "    public int32 displayName;\n"
        "    @JsonIgnore\n"
        "    public int32 secret;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Mix a = heap Mix();\n"
        "        a.id = 1;\n"
        "        a.displayName = 2;\n"
        "        a.secret = 999;\n"
        "        int8[] bytes = Json.toBytes<Mix>(a);\n"
        "        int64 n = (int64) bytes.count();\n"
        "        Mix b = Json.parse<Mix>(bytes, n);\n"
        // b.id and b.displayName round-trip; b.secret stays 0
        "        return b.id * 100 + b.displayName * 10 + b.secret;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 120);  // 100 + 20 + 0
}

// ---- Phase 4b commit 11: String-typed user-API overloads ----

// `JsonObject.get(String)` — convenience overload that delegates to
// the byte-buffer form against the String's `.bytes` + `.byteLength()`.
TEST(JsonSynthesizerTests, jsonObjectGetByString) {
    auto src =
        "package test;\n"
        "import cajeta.codec.json.Json;\n"
        "import cajeta.codec.json.JsonValue;\n"
        "import cajeta.codec.json.JsonObject;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        String s = \"{\\\"id\\\":42}\";\n"
        "        JsonValue v = Json.parse(s);\n"
        "        JsonObject o = v.asObject();\n"
        "        JsonValue idValue = o.get(\"id\");\n"
        "        return idValue.asInt32();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// `JsonReader.currentString()` materializes a String view of the
// current token's bytes.
TEST(JsonSynthesizerTests, jsonReaderCurrentString) {
    auto src =
        "package test;\n"
        "import cajeta.codec.json.JsonReader;\n"
        "import cajeta.codec.json.JsonToken;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        String s = \"\\\"hi\\\"\";\n"
        "        int8[] sb = s.toBytes();\n"
        "        JsonReader r = heap JsonReader(sb, (int64) s.byteLength());\n"
        "        int32 t = r.next();\n"
        "        if (t != JsonToken.STRING) return 0;\n"
        "        String got = r.currentString();\n"
        "        return (int32) got.count();\n"  // 2 codepoints
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 2);
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
        "        int8[] buf = heap int8[26];\n"
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
        "        int8[] buf2 = heap int8[27];\n"
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

// ---- Phase 4b commit 10: nested-class arrays via JsonReader.peek() ----

// Parse `{"items":[{"x":1},{"x":2}]}` into Wrap { Inner[] items; }
// with Inner { int32 x; }. Pins peek + dispatch — the array
// reader peeks each element's START_OBJECT and hands the reader
// (still pointing at START_OBJECT) to parseObjectFromReader<T>.
TEST(JsonSynthesizerTests, parseNestedClassArray) {
    auto src =
        "package test;\n"
        "import cajeta.codec.json.Json;\n"
        "public class Inner { public int32 x; }\n"
        "public class Wrap { public Inner[] items; }\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        String s = \"{\\\"items\\\":[{\\\"x\\\":3},{\\\"x\\\":5}]}\";\n"
        "        Wrap w = Json.parse<Wrap>(s);\n"
        "        return w.items[0].x * 100 + w.items[1].x;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 305);
}

// Empty nested-class array — `{"items":[]}` → 0 elements.
TEST(JsonSynthesizerTests, parseEmptyNestedClassArray) {
    auto src =
        "package test;\n"
        "import cajeta.codec.json.Json;\n"
        "public class Inner { public int32 x; }\n"
        "public class Wrap { public Inner[] items; }\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        String s = \"{\\\"items\\\":[]}\";\n"
        "        Wrap w = Json.parse<Wrap>(s);\n"
        "        return (int32) w.items.count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 0);
}

// Round-trip nested-class array — write + parse symmetric.
TEST(JsonSynthesizerTests, roundTripNestedClassArray) {
    auto src =
        "package test;\n"
        "import cajeta.codec.json.Json;\n"
        "public class Inner { public int32 x; }\n"
        "public class Wrap { public Inner[] items; }\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Wrap a = heap Wrap();\n"
        "        a.items = heap Inner[2];\n"
        "        a.items[0] = heap Inner();\n"
        "        a.items[0].x = 7;\n"
        "        a.items[1] = heap Inner();\n"
        "        a.items[1].x = 9;\n"
        "        int8[] bytes = Json.toBytes<Wrap>(a);\n"
        "        int64 n = (int64) bytes.count();\n"
        "        Wrap b = Json.parse<Wrap>(bytes, n);\n"
        "        return b.items[0].x * 10 + b.items[1].x;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 79);
}

// ---- Phase 4b commit 9: @JsonRequired ----

// Required field present in input — parses normally, no throw.
TEST(JsonSynthesizerTests, jsonRequiredFieldPresent) {
    auto src =
        "package test;\n"
        "import cajeta.codec.json.Json;\n"
        "public class Box {\n"
        "    @JsonRequired\n"
        "    public int32 id;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        String s = \"{\\\"id\\\":7}\";\n"
        "        Box b = Json.parse<Box>(s);\n"
        "        return b.id;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

// Required field MISSING — parse throws JsonParseException naming
// the field. The test catches via the JIT's exception machinery
// (Cajeta.lastThrownErrorId()-style tagging) and verifies the
// error id matches the synthesizer's CAJETA_ERROR_JSON_REQUIRED.
TEST(JsonSynthesizerTests, jsonRequiredFieldMissing) {
    auto src =
        "package test;\n"
        "import cajeta.codec.json.Json;\n"
        "public class Box {\n"
        "    @JsonRequired\n"
        "    public int32 id;\n"
        "    public int32 other;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        // JSON contains `other` but not `id`. Parse should throw.
        "        String s = \"{\\\"other\\\":99}\";\n"
        "        try {\n"
        "            Box b = Json.parse<Box>(s);\n"
        "            return 0;\n"   // no throw — fail
        "        } catch (cajeta.codec.json.JsonParseException e) {\n"
        "            return 1;\n"   // expected throw
        "        }\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// Two required fields, one present, one missing — still throws.
TEST(JsonSynthesizerTests, jsonRequiredOnePresentOneMissing) {
    auto src =
        "package test;\n"
        "import cajeta.codec.json.Json;\n"
        "public class Pair {\n"
        "    @JsonRequired\n"
        "    public int32 left;\n"
        "    @JsonRequired\n"
        "    public int32 right;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        String s = \"{\\\"left\\\":1}\";\n"
        "        try {\n"
        "            Pair p = Json.parse<Pair>(s);\n"
        "            return 0;\n"
        "        } catch (cajeta.codec.json.JsonParseException e) {\n"
        "            return 1;\n"
        "        }\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// ---- Phase 4b commit 10: @JsonAlias({...}) ----

// Field accepts the alias key on read. Write-side still uses the
// primary key (declared name or @JsonProperty rename).
TEST(JsonSynthesizerTests, jsonAliasReadsAlternateKey) {
    auto src =
        "package test;\n"
        "import cajeta.codec.json.Json;\n"
        "public class Box {\n"
        "    @JsonAlias({\"userId\", \"user-id\"})\n"
        "    public int32 id;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        // Wire key is the second alias — userId. id (declared name)
        // and user-id both also work, but pick the un-declared alias
        // to prove it's not just falling back on the declared name.
        "        String s = \"{\\\"userId\\\":42}\";\n"
        "        Box b = Json.parse<Box>(s);\n"
        "        return b.id;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// Primary (declared) key still works alongside aliases.
TEST(JsonSynthesizerTests, jsonAliasPrimaryStillWorks) {
    auto src =
        "package test;\n"
        "import cajeta.codec.json.Json;\n"
        "public class Box {\n"
        "    @JsonAlias({\"userId\"})\n"
        "    public int32 id;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        String s = \"{\\\"id\\\":7}\";\n"
        "        Box b = Json.parse<Box>(s);\n"
        "        return b.id;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

// ---- Phase 4b commit 11: @JsonInclude(NON_NULL) ----

// Reference-typed field with @JsonInclude(NON_NULL) — when the
// reference IS null, the key/value pair is omitted from output.
// Result: `{}` for a class with one null-valued NON_NULL field.
TEST(JsonSynthesizerTests, jsonIncludeNonNullOmitsNullReference) {
    auto src =
        "package test;\n"
        "import cajeta.codec.json.Json;\n"
        "import cajeta.lang.String;\n"
        "public class Box {\n"
        "    @JsonInclude(\"NON_NULL\")\n"
        "    public String name;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Box a = heap Box();\n"
        "        a.name = null;\n"
        "        int8[] bytes = Json.toBytes<Box>(a);\n"
        "        return (int32) bytes.count();\n"  // 2: just `{}`
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 2);
}

// Same field, non-null reference — emitted normally.
TEST(JsonSynthesizerTests, jsonIncludeNonNullKeepsValue) {
    auto src =
        "package test;\n"
        "import cajeta.codec.json.Json;\n"
        "import cajeta.lang.String;\n"
        "public class Box {\n"
        "    @JsonInclude(\"NON_NULL\")\n"
        "    public String name;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Box a = heap Box();\n"
        "        a.name = \"hi\";\n"
        "        int8[] bytes = Json.toBytes<Box>(a);\n"
        // {"name":"hi"} → 13 bytes
        "        return (int32) bytes.count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 13);
}

// NON_NULL on an array field with null reference — omitted.
TEST(JsonSynthesizerTests, jsonIncludeNonNullArray) {
    auto src =
        "package test;\n"
        "import cajeta.codec.json.Json;\n"
        "public class Bag {\n"
        "    @JsonInclude(\"NON_NULL\")\n"
        "    public int32[] ids;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Bag a = heap Bag();\n"
        "        a.ids = null;\n"
        "        int8[] bytes = Json.toBytes<Bag>(a);\n"
        "        return (int32) bytes.count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 2);
}

// @JsonInclude("NEVER") — always omit on write. The field's value
// is preserved in memory; the writer just never echoes it.
TEST(JsonSynthesizerTests, jsonIncludeNeverOmitsFromWrite) {
    auto src =
        "package test;\n"
        "import cajeta.codec.json.Json;\n"
        "public class Audit {\n"
        "    public int32 id;\n"
        "    @JsonInclude(\"NEVER\")\n"
        "    public int32 internalHash;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Audit a = heap Audit();\n"
        "        a.id = 1;\n"
        "        a.internalHash = 42;\n"
        "        int8[] bytes = Json.toBytes<Audit>(a);\n"
        // {"id":1} → 8 bytes; internalHash never written.
        "        return (int32) bytes.count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 8);
}

// @JsonInclude("NON_DEFAULT") on primitives — omit when zero.
TEST(JsonSynthesizerTests, jsonIncludeNonDefaultOmitsZeroInt) {
    auto src =
        "package test;\n"
        "import cajeta.codec.json.Json;\n"
        "public class Counters {\n"
        "    public int32 keep;\n"
        "    @JsonInclude(\"NON_DEFAULT\")\n"
        "    public int32 maybe;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Counters c = heap Counters();\n"
        "        c.keep = 1;\n"
        "        c.maybe = 0;\n"  // default → omitted
        "        int8[] bytes = Json.toBytes<Counters>(c);\n"
        // {"keep":1} → 10 bytes; maybe omitted because == 0.
        "        return (int32) bytes.count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 10);
}

// @JsonInclude("NON_DEFAULT") on primitives — emit when non-default.
TEST(JsonSynthesizerTests, jsonIncludeNonDefaultKeepsNonZeroInt) {
    auto src =
        "package test;\n"
        "import cajeta.codec.json.Json;\n"
        "public class Counters {\n"
        "    public int32 keep;\n"
        "    @JsonInclude(\"NON_DEFAULT\")\n"
        "    public int32 maybe;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Counters c = heap Counters();\n"
        "        c.keep = 1;\n"
        "        c.maybe = 7;\n"
        "        int8[] bytes = Json.toBytes<Counters>(c);\n"
        // {"keep":1,"maybe":7} → 20 bytes.
        "        return (int32) bytes.count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 20);
}

// @JsonInclude("NON_DEFAULT") on a class-ref / array / String —
// behavior collapses to NON_NULL since the type default is null.
TEST(JsonSynthesizerTests, jsonIncludeNonDefaultOnReferenceCollapsesToNonNull) {
    auto src =
        "package test;\n"
        "import cajeta.codec.json.Json;\n"
        "public class Bag {\n"
        "    @JsonInclude(\"NON_DEFAULT\")\n"
        "    public int32[] ids;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Bag a = heap Bag();\n"
        "        a.ids = null;\n"
        "        int8[] bytes = Json.toBytes<Bag>(a);\n"
        // Empty object {} → 2 bytes.
        "        return (int32) bytes.count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 2);
}

// ---- Phase 4b commit 12: class-level @JsonNamingStrategy + @JsonStrict ----

// Class-level @JsonNamingStrategy("SNAKE_CASE") renames every field
// for the wire by converting camelCase → snake_case. Per-field
// @JsonProperty still wins when present.
TEST(JsonSynthesizerTests, jsonNamingStrategySnakeCase) {
    auto src =
        "package test;\n"
        "import cajeta.codec.json.Json;\n"
        "@JsonNamingStrategy(\"SNAKE_CASE\")\n"
        "public class User {\n"
        "    public int32 firstName;\n"
        "    public int32 lastName;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        User a = heap User();\n"
        "        a.firstName = 1;\n"
        "        a.lastName = 2;\n"
        "        int8[] bytes = Json.toBytes<User>(a);\n"
        // {"first_name":1,"last_name":2} → 30 bytes
        "        return (int32) bytes.count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 30);
}

// KEBAB_CASE writes wire keys with `-` separators.
TEST(JsonSynthesizerTests, jsonNamingStrategyKebabCase) {
    auto src =
        "package test;\n"
        "import cajeta.codec.json.Json;\n"
        "@JsonNamingStrategy(\"KEBAB_CASE\")\n"
        "public class User {\n"
        "    public int32 firstName;\n"
        "    public int32 lastName;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        User a = heap User();\n"
        "        a.firstName = 1;\n"
        "        a.lastName = 2;\n"
        "        int8[] bytes = Json.toBytes<User>(a);\n"
        // {"first-name":1,"last-name":2} → 30 bytes
        "        return (int32) bytes.count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 30);
}

// PASCAL_CASE capitalizes the first letter. `firstName` → `FirstName`.
TEST(JsonSynthesizerTests, jsonNamingStrategyPascalCase) {
    auto src =
        "package test;\n"
        "import cajeta.codec.json.Json;\n"
        "@JsonNamingStrategy(\"PASCAL_CASE\")\n"
        "public class User {\n"
        "    public int32 firstName;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        User a = heap User();\n"
        "        a.firstName = 7;\n"
        "        int8[] bytes = Json.toBytes<User>(a);\n"
        // {"FirstName":7} → 15 bytes
        "        return (int32) bytes.count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 15);
}

// PASCAL_CASE on the read side accepts the capitalized wire key.
TEST(JsonSynthesizerTests, jsonNamingStrategyPascalCaseRead) {
    auto src =
        "package test;\n"
        "import cajeta.codec.json.Json;\n"
        "@JsonNamingStrategy(\"PASCAL_CASE\")\n"
        "public class User {\n"
        "    public int32 firstName;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        String s = \"{\\\"FirstName\\\":42}\";\n"
        "        User u = Json.parse<User>(s);\n"
        "        return u.firstName;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// CAMEL_CASE is identity for cajeta's camelCase field convention —
// declaring it makes the contract explicit at the class level.
TEST(JsonSynthesizerTests, jsonNamingStrategyCamelCaseIsIdentity) {
    auto src =
        "package test;\n"
        "import cajeta.codec.json.Json;\n"
        "@JsonNamingStrategy(\"CAMEL_CASE\")\n"
        "public class User {\n"
        "    public int32 firstName;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        String s = \"{\\\"firstName\\\":5}\";\n"
        "        User u = Json.parse<User>(s);\n"
        "        return u.firstName;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 5);
}

// IDENTITY strategy — explicit no-op. Same as omitting the
// annotation entirely; pinned here so a user reviewing class-level
// policy can write the no-transform case explicitly.
TEST(JsonSynthesizerTests, jsonNamingStrategyIdentity) {
    auto src =
        "package test;\n"
        "import cajeta.codec.json.Json;\n"
        "@JsonNamingStrategy(\"IDENTITY\")\n"
        "public class User {\n"
        "    public int32 firstName;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        String s = \"{\\\"firstName\\\":9}\";\n"
        "        User u = Json.parse<User>(s);\n"
        "        return u.firstName;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 9);
}

// SNAKE_CASE renames work on the READ side too. Confirms the
// renamed wire key matches.
TEST(JsonSynthesizerTests, jsonNamingStrategySnakeCaseRead) {
    auto src =
        "package test;\n"
        "import cajeta.codec.json.Json;\n"
        "@JsonNamingStrategy(\"SNAKE_CASE\")\n"
        "public class User {\n"
        "    public int32 firstName;\n"
        "    public int32 lastName;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        String s = \"{\\\"first_name\\\":3,\\\"last_name\\\":4}\";\n"
        "        User u = Json.parse<User>(s);\n"
        "        return u.firstName * 10 + u.lastName;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 34);
}

// Per-field @JsonProperty overrides the class-level naming strategy.
TEST(JsonSynthesizerTests, jsonPropertyOverridesNamingStrategy) {
    auto src =
        "package test;\n"
        "import cajeta.codec.json.Json;\n"
        "@JsonNamingStrategy(\"SNAKE_CASE\")\n"
        "public class User {\n"
        "    public int32 firstName;\n"
        "    @JsonProperty(\"surname\")\n"
        "    public int32 lastName;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        User a = heap User();\n"
        "        a.firstName = 0;\n"
        "        a.lastName = 0;\n"
        "        int8[] bytes = Json.toBytes<User>(a);\n"
        // {"first_name":0,"surname":0} → 28 bytes
        "        return (int32) bytes.count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 28);
}

// Class-level @JsonStrict rejects unknown keys on read instead of
// silently skipping them. Bare class behaves as before (skips).
TEST(JsonSynthesizerTests, jsonStrictRejectsUnknownKeys) {
    auto src =
        "package test;\n"
        "import cajeta.codec.json.Json;\n"
        "@JsonStrict\n"
        "public class Box {\n"
        "    public int32 id;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        String s = \"{\\\"id\\\":1,\\\"unknown\\\":2}\";\n"
        "        try {\n"
        "            Box b = Json.parse<Box>(s);\n"
        "            return 0;\n"
        "        } catch (cajeta.codec.json.JsonParseException e) {\n"
        "            return 1;\n"
        "        }\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// Without @JsonStrict, unknown keys are silently consumed (v1
// behavior — skipValue arm). Confirms strict really is opt-in.
TEST(JsonSynthesizerTests, withoutJsonStrictUnknownKeysSkipped) {
    auto src =
        "package test;\n"
        "import cajeta.codec.json.Json;\n"
        "public class Box {\n"
        "    public int32 id;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        String s = \"{\\\"id\\\":7,\\\"unknown\\\":9}\";\n"
        "        Box b = Json.parse<Box>(s);\n"
        "        return b.id;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

// Required + renamed key — annotations compose. Required tracks
// the WIRE key (set by @JsonProperty), not the declared name.
TEST(JsonSynthesizerTests, jsonRequiredWithRename) {
    auto src =
        "package test;\n"
        "import cajeta.codec.json.Json;\n"
        "public class Box {\n"
        "    @JsonRequired\n"
        "    @JsonProperty(\"user_id\")\n"
        "    public int32 id;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        String s = \"{\\\"user_id\\\":42}\";\n"
        "        Box b = Json.parse<Box>(s);\n"
        "        return b.id;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// ---- Optional<T> inner types beyond int32/String (4.5) ----
//
// The synthesizer emits a distinct read + empty-default pair per inner
// type; only the int32 and String arms had pins.

TEST(JsonSynthesizerTests, jsonOptionalInt64PresentAndNull) {
    auto src =
        "package test;\n"
        "import cajeta.codec.json.Json;\n"
        "import cajeta.lang.Optional;\n"
        "public class WithOpt {\n"
        "    public Optional<int64> n;\n"
        "}\n"
        "public final class D {\n"
        "    public static int64 run() {\n"
        "        String present = \"{\\\"n\\\":1234567890123}\";\n"
        "        WithOpt a = Json.parse<WithOpt>(present);\n"
        "        if (a.n == null) { return (int64) -1; }\n"
        "        if (a.n.isEmpty()) { return (int64) -2; }\n"
        "        String nulled = \"{\\\"n\\\":null}\";\n"
        "        WithOpt b = Json.parse<WithOpt>(nulled);\n"
        "        if (b.n == null) { return (int64) -3; }\n"
        "        if (!b.n.isEmpty()) { return (int64) -4; }\n"
        "        return a.n.get();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI64(src), 1234567890123LL);
}

TEST(JsonSynthesizerTests, jsonOptionalBooleanPresentAndNull) {
    auto src =
        "package test;\n"
        "import cajeta.codec.json.Json;\n"
        "import cajeta.lang.Optional;\n"
        "public class WithOpt {\n"
        "    public Optional<boolean> flag;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        String present = \"{\\\"flag\\\":true}\";\n"
        "        WithOpt a = Json.parse<WithOpt>(present);\n"
        "        if (a.flag == null) { return -1; }\n"
        "        if (a.flag.isEmpty()) { return -2; }\n"
        "        if (!a.flag.get()) { return -3; }\n"
        "        String nulled = \"{\\\"flag\\\":null}\";\n"
        "        WithOpt b = Json.parse<WithOpt>(nulled);\n"
        "        if (!b.flag.isEmpty()) { return -4; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

TEST(JsonSynthesizerTests, jsonOptionalFloat64PresentAndNull) {
    auto src =
        "package test;\n"
        "import cajeta.codec.json.Json;\n"
        "import cajeta.lang.Optional;\n"
        "public class WithOpt {\n"
        "    public Optional<float64> x;\n"
        "}\n"
        "public final class D {\n"
        "    public static float64 run() {\n"
        "        String nulled = \"{\\\"x\\\":null}\";\n"
        "        WithOpt b = Json.parse<WithOpt>(nulled);\n"
        "        if (b.x == null) { return -1.0; }\n"
        "        if (!b.x.isEmpty()) { return -2.0; }\n"
        "        String present = \"{\\\"x\\\":2.5}\";\n"
        "        WithOpt a = Json.parse<WithOpt>(present);\n"
        "        if (a.x.isEmpty()) { return -3.0; }\n"
        "        return a.x.get();\n"
        "    }\n"
        "}\n";
    EXPECT_NEAR(runF64(src), 2.5, 1e-9);
}

// @JsonRequired composes with a naming strategy: the required-flag arm
// must key off the EFFECTIVE wire key, not the declared field name.
TEST(JsonSynthesizerTests, jsonRequiredUnderNamingStrategy) {
    auto src =
        "package test;\n"
        "import cajeta.codec.json.Json;\n"
        "@JsonNamingStrategy(\"SNAKE_CASE\")\n"
        "public class Box {\n"
        "    @JsonRequired public int32 userId;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        String s = \"{\\\"user_id\\\":42}\";\n"
        "        Box b = Json.parse<Box>(s);\n"
        "        return b.userId;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}
