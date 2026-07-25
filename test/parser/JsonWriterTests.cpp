// JsonWriter Phase 2 tests. Each test builds a small document via
// the fluent writer API and verifies the emitted bytes by reading
// them through JsonReader (round-trip) or by inspecting the byte
// count + first few bytes directly.

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

constexpr const char* PRELUDE =
    "package test;\n"
    "import cajeta.codec.json.JsonWriter;\n"
    "import cajeta.codec.json.JsonReader;\n";
} // namespace

// Empty object emit: writer produces `{}` (2 bytes).
TEST(JsonWriterTests, emptyObjectByteCount) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        JsonWriter w = heap JsonWriter();\n"
        "        w.beginObject().endObject();\n"
        "        int8[] out = w.toBytes();\n"
        "        return (int32) out[0] * 100 + (int32) out[1];\n"
        "    }\n"
        "}\n";
    // '{' = 123, '}' = 125 → 123*100 + 125 = 12425
    EXPECT_EQ(runI32(src), 12425);
}

// Empty array emit: writer produces `[]`.
TEST(JsonWriterTests, emptyArrayByteCount) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        JsonWriter w = heap JsonWriter();\n"
        "        w.beginArray().endArray();\n"
        "        int8[] out = w.toBytes();\n"
        "        return (int32) out[0] * 100 + (int32) out[1];\n"
        "    }\n"
        "}\n";
    // '[' = 91, ']' = 93 → 91*100 + 93 = 9193
    EXPECT_EQ(runI32(src), 9193);
}

// Array of three numbers: [1,2,3] (7 bytes). Verify size + the
// digits at the expected offsets.
TEST(JsonWriterTests, arrayThreeNumbers) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        JsonWriter w = heap JsonWriter();\n"
        "        w.beginArray()\n"
        "            .writeNumber((int64) 1)\n"
        "            .writeNumber((int64) 2)\n"
        "            .writeNumber((int64) 3)\n"
        "         .endArray();\n"
        "        int32 sz = w.size();\n"
        "        if (sz != 7) { return -sz; }\n"
        "        int8[] out = w.toBytes();\n"
        "        // expected bytes: '[','1',',','2',',','3',']'\n"
        "        if (out[0] != (int8) 91)  { return -10; }\n"
        "        if (out[1] != (int8) 49)  { return -11; }\n"
        "        if (out[2] != (int8) 44)  { return -12; }\n"
        "        if (out[3] != (int8) 50)  { return -13; }\n"
        "        if (out[4] != (int8) 44)  { return -14; }\n"
        "        if (out[5] != (int8) 51)  { return -15; }\n"
        "        if (out[6] != (int8) 93)  { return -16; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// Larger integer formatting: -1234567890 → "-1234567890" (11 bytes).
TEST(JsonWriterTests, negativeLargeInteger) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        JsonWriter w = heap JsonWriter();\n"
        "        w.writeNumber((int64) -1234567890);\n"
        "        int32 sz = w.size();\n"
        "        if (sz != 11) { return -sz; }\n"
        "        int8[] out = w.toBytes();\n"
        "        if (out[0] != (int8) 45)  { return -1; }\n"  // '-'
        "        if (out[1] != (int8) 49)  { return -2; }\n"  // '1'
        "        if (out[10] != (int8) 48) { return -3; }\n"  // '0'
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// Booleans + null: [true,false,null] (17 bytes).
TEST(JsonWriterTests, booleansAndNull) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        JsonWriter w = heap JsonWriter();\n"
        "        w.beginArray()\n"
        "            .writeBoolean(true)\n"
        "            .writeBoolean(false)\n"
        "            .writeNull()\n"
        "         .endArray();\n"
        "        return w.size();\n"
        "    }\n"
        "}\n";
    // [true,false,null] = 17 chars
    EXPECT_EQ(runI32(src), 17);
}

// Object with single string-valued key: {"a":"b"} — 9 bytes
// (`{`, `"`, `a`, `"`, `:`, `"`, `b`, `"`, `}`).
TEST(JsonWriterTests, objectWithStringValue) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int8[] k = [(int8) 97];\n"   // "a"
        "        int8[] v = [(int8) 98];\n"   // "b"
        "        JsonWriter w = heap JsonWriter();\n"
        "        w.beginObject()\n"
        "            .key(k, 1).writeString(v, 1)\n"
        "         .endObject();\n"
        "        return w.size();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 9);
}

// Round-trip: writer emits `[1,2,3]` and reader walks it back.
TEST(JsonWriterTests, roundTripWithReader) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        JsonWriter w = heap JsonWriter();\n"
        "        w.beginArray()\n"
        "            .writeNumber((int64) 10)\n"
        "            .writeNumber((int64) 20)\n"
        "            .writeNumber((int64) 30)\n"
        "         .endArray();\n"
        "        int32 sz = w.size();\n"
        "        int8[] bytes = w.toBytes();\n"
        "        JsonReader r = heap JsonReader(bytes, (int64) sz);\n"
        "        int32 sum = 0;\n"
        "        sum = sum + r.next();\n"  // 2 START_ARRAY
        "        sum = sum + r.next();\n"  // 6 NUMBER (10)
        "        int64 a = r.currentNumberAsInt64();\n"
        "        sum = sum + (int32) a;\n" // +10
        "        sum = sum + r.next();\n"  // 6 NUMBER
        "        sum = sum + (int32) r.currentNumberAsInt64();\n" // +20
        "        sum = sum + r.next();\n"  // 6
        "        sum = sum + (int32) r.currentNumberAsInt64();\n" // +30
        "        sum = sum + r.next();\n"  // 3 END_ARRAY
        "        return sum;\n"
        "    }\n"
        "}\n";
    // 2 + 6 + 10 + 6 + 20 + 6 + 30 + 3 = 83
    EXPECT_EQ(runI32(src), 83);
}

// currentNumberAsInt64 negative parse.
TEST(JsonReaderNumberTests, parseNegativeInt) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int8[] buf = [(int8) 45, (int8) 52, (int8) 50];\n"  // -42
        "        JsonReader r = heap JsonReader(buf, (int64) 3);\n"
        "        r.next();\n"
        "        return (int32) r.currentNumberAsInt64();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), -42);
}

// currentNumberAsInt64 throws JsonParseException on number > INT64_MAX
// (regression: under --overflow-checks=on the digit-accumulation
// multiplication previously trapped with SIGILL).
TEST(JsonReaderNumberTests, parseTooLargeInt64ThrowsParseException) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        // "9223372036854775808" — INT64_MAX + 1
        "        int8[] buf = [\n"
        "            (int8) 57, (int8) 50, (int8) 50, (int8) 51,\n"
        "            (int8) 51, (int8) 55, (int8) 50, (int8) 48,\n"
        "            (int8) 51, (int8) 54, (int8) 56, (int8) 53,\n"
        "            (int8) 52, (int8) 55, (int8) 55, (int8) 53,\n"
        "            (int8) 56, (int8) 48, (int8) 56\n"
        "        ];\n"
        "        JsonReader r = heap JsonReader(buf, (int64) 19);\n"
        "        r.next();\n"
        "        try {\n"
        "            r.currentNumberAsInt64();\n"
        "            return 0;\n"  // didn't throw → fail
        "        } catch (JsonParseException e) {\n"
        "            return 1;\n"
        "        }\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// currentNumberAsInt64 parses INT64_MIN cleanly (the one negative
// whose absolute value exceeds INT64_MAX). Previously CreateNeg on
// INT64_MAX+1-equivalent would trap.
TEST(JsonReaderNumberTests, parseInt64Min) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int64 run() {\n"
        // "-9223372036854775808" — INT64_MIN
        "        int8[] buf = [\n"
        "            (int8) 45,\n"  // '-'
        "            (int8) 57, (int8) 50, (int8) 50, (int8) 51,\n"
        "            (int8) 51, (int8) 55, (int8) 50, (int8) 48,\n"
        "            (int8) 51, (int8) 54, (int8) 56, (int8) 53,\n"
        "            (int8) 52, (int8) 55, (int8) 55, (int8) 53,\n"
        "            (int8) 56, (int8) 48, (int8) 56\n"
        "        ];\n"
        "        JsonReader r = heap JsonReader(buf, (int64) 20);\n"
        "        r.next();\n"
        "        return r.currentNumberAsInt64();\n"
        "    }\n"
        "}\n";
    auto jit = cajeta_test::CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int64_t (*)()>("run");
    EXPECT_EQ(fn(), INT64_MIN);
}

// currentBytes() returns a fresh int8[] copy of the current span.
TEST(JsonReaderBytesTests, currentBytesCopiesKeySpan) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        // {"ab":1}
        "        int8[] buf = [(int8) 123, (int8) 34, (int8) 97, (int8) 98,\n"
        "                      (int8) 34, (int8) 58, (int8) 49, (int8) 125];\n"
        "        JsonReader r = heap JsonReader(buf, (int64) 8);\n"
        "        r.next();\n"  // START_OBJECT
        "        r.next();\n"  // KEY
        "        int8[] k = r.currentBytes();\n"
        "        // k must be 2 bytes: 'a','b'\n"
        "        return (int32) k[0] * 100 + (int32) k[1];\n"
        "    }\n"
        "}\n";
    // 'a'=97, 'b'=98 → 9798
    EXPECT_EQ(runI32(src), 9798);
}
