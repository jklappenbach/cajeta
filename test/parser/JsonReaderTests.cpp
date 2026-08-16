// JsonReader Phase 1 pull-tokenizer tests. Each test feeds a JSON
// literal as an int8[] and walks JsonReader.next() to verify the
// expected token sequence + byte-span recording. String / number
// materialization (Phase 2) and the JsonValue tree (Phase 3) are
// out of scope.

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

// Common preamble: import the reader and stash the input bytes
// into a fresh int8[] inside run(). Pads the buffer with the JSON
// text + a trailing 0 so the test source reads cleanly.
constexpr const char* PRELUDE =
    "package test;\n"
    "import cajeta.codec.json.JsonReader;\n";
} // namespace

// `{}` — START_OBJECT (0), END_OBJECT (1), END (9).
TEST(JsonReaderTests, emptyObject) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int8[] buf = [(int8) 123, (int8) 125];\n"  // '{', '}'
        "        JsonReader r = heap JsonReader(buf, (int64) 2);\n"
        "        int32 t1 = r.next();\n"
        "        int32 t2 = r.next();\n"
        "        int32 t3 = r.next();\n"
        "        return t1 * 100 + t2 * 10 + t3;\n"
        "    }\n"
        "}\n";
    // 0 START_OBJECT, 1 END_OBJECT, 9 END → 0*100 + 1*10 + 9 = 19
    EXPECT_EQ(runI32(src), 19);
}

// `[]` — START_ARRAY (2), END_ARRAY (3), END (9).
TEST(JsonReaderTests, emptyArray) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int8[] buf = [(int8) 91, (int8) 93];\n"  // '[', ']'
        "        JsonReader r = heap JsonReader(buf, (int64) 2);\n"
        "        int32 t1 = r.next();\n"
        "        int32 t2 = r.next();\n"
        "        int32 t3 = r.next();\n"
        "        return t1 * 100 + t2 * 10 + t3;\n"
        "    }\n"
        "}\n";
    // 2 START_ARRAY, 3 END_ARRAY, 9 END → 2*100 + 3*10 + 9 = 239
    EXPECT_EQ(runI32(src), 239);
}

// `true`  — BOOLEAN (7) with currentBoolean() == true.
TEST(JsonReaderTests, literalTrue) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        // 't','r','u','e'
        "        int8[] buf = [(int8) 116, (int8) 114, (int8) 117, (int8) 101];\n"
        "        JsonReader r = heap JsonReader(buf, (int64) 4);\n"
        "        int32 t = r.next();\n"
        "        if (t != 7) { return -1; }\n"
        "        if (!r.currentBoolean()) { return -2; }\n"
        "        if (r.next() != 9) { return -3; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// `false` — BOOLEAN (7) with currentBoolean() == false.
TEST(JsonReaderTests, literalFalse) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        // 'f','a','l','s','e'
        "        int8[] buf = [(int8) 102, (int8) 97, (int8) 108, (int8) 115, (int8) 101];\n"
        "        JsonReader r = heap JsonReader(buf, (int64) 5);\n"
        "        int32 t = r.next();\n"
        "        if (t != 7) { return -1; }\n"
        "        if (r.currentBoolean()) { return -2; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// `null`  — NULL (8).
TEST(JsonReaderTests, literalNull) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        // 'n','u','l','l'
        "        int8[] buf = [(int8) 110, (int8) 117, (int8) 108, (int8) 108];\n"
        "        JsonReader r = heap JsonReader(buf, (int64) 4);\n"
        "        return r.next();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 8);
}

// Naked integer `42` — NUMBER (6), span covers both digits.
TEST(JsonReaderTests, nakedNumberSpan) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        // '4','2'
        "        int8[] buf = [(int8) 52, (int8) 50];\n"
        "        JsonReader r = heap JsonReader(buf, (int64) 2);\n"
        "        int32 t = r.next();\n"
        "        if (t != 6) { return -1; }\n"
        "        int32 span = (int32) (r.tokenEnd() - r.tokenStart());\n"
        "        if (span != 2) { return -2; }\n"
        "        return (int32) r.tokenStart() * 10 + span;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 2);   // start=0, span=2 → 0*10+2 = 2
}

// `[1,2,3]` — array with three numbers + separators.
TEST(JsonReaderTests, arrayOfThreeNumbers) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        // '[','1',',','2',',','3',']'
        "        int8[] buf = [(int8) 91, (int8) 49, (int8) 44,\n"
        "                      (int8) 50, (int8) 44, (int8) 51, (int8) 93];\n"
        "        JsonReader r = heap JsonReader(buf, (int64) 7);\n"
        "        int32 sum = 0;\n"
        "        sum = sum + r.next();\n"  // 2 START_ARRAY
        "        sum = sum + r.next();\n"  // 6 NUMBER
        "        sum = sum + r.next();\n"  // 6
        "        sum = sum + r.next();\n"  // 6
        "        sum = sum + r.next();\n"  // 3 END_ARRAY
        "        sum = sum + r.next();\n"  // 9 END
        "        return sum;\n"
        "    }\n"
        "}\n";
    // 2+6+6+6+3+9 = 32
    EXPECT_EQ(runI32(src), 32);
}

// `{"a":1}` — single-key object: START_OBJECT, KEY, NUMBER, END_OBJECT, END.
TEST(JsonReaderTests, objectWithOneNumericValue) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        // '{','"','a','"',':','1','}'
        "        int8[] buf = [(int8) 123, (int8) 34, (int8) 97, (int8) 34,\n"
        "                      (int8) 58, (int8) 49, (int8) 125];\n"
        "        JsonReader r = heap JsonReader(buf, (int64) 7);\n"
        "        int32 t1 = r.next();\n"  // 0 START_OBJECT
        "        int32 t2 = r.next();\n"  // 4 KEY
        "        int32 t3 = r.next();\n"  // 6 NUMBER
        "        int32 t4 = r.next();\n"  // 1 END_OBJECT
        "        return t1 * 1000 + t2 * 100 + t3 * 10 + t4;\n"
        "    }\n"
        "}\n";
    // 0*1000 + 4*100 + 6*10 + 1 = 461
    EXPECT_EQ(runI32(src), 461);
}

// Nested array `[[]]`. Outer + inner brackets balance correctly.
TEST(JsonReaderTests, nestedEmptyArray) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        // '[','[',']',']'
        "        int8[] buf = [(int8) 91, (int8) 91, (int8) 93, (int8) 93];\n"
        "        JsonReader r = heap JsonReader(buf, (int64) 4);\n"
        "        int32 t1 = r.next();\n"  // 2 START_ARRAY
        "        int32 t2 = r.next();\n"  // 2 START_ARRAY
        "        int32 t3 = r.next();\n"  // 3 END_ARRAY
        "        int32 t4 = r.next();\n"  // 3 END_ARRAY
        "        return t1 * 1000 + t2 * 100 + t3 * 10 + t4;\n"
        "    }\n"
        "}\n";
    // 2*1000 + 2*100 + 3*10 + 3 = 2233
    EXPECT_EQ(runI32(src), 2233);
}

// Whitespace tolerance: `  { } ` — leading + interior whitespace
// shouldn't affect token output.
TEST(JsonReaderTests, whitespaceSkipped) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        // ' ',' ','{',' ','}',' '
        "        int8[] buf = [(int8) 32, (int8) 32, (int8) 123,\n"
        "                      (int8) 32, (int8) 125, (int8) 32];\n"
        "        JsonReader r = heap JsonReader(buf, (int64) 6);\n"
        "        int32 t1 = r.next();\n"
        "        int32 t2 = r.next();\n"
        "        int32 t3 = r.next();\n"
        "        return t1 * 100 + t2 * 10 + t3;\n"
        "    }\n"
        "}\n";
    // 0,1,9 → 19 (same as emptyObject)
    EXPECT_EQ(runI32(src), 19);
}

// ── decoded-string accessor (cajeta-llama spec 7.8, 13.7) ──────────────
// `currentDecodedBytes()` returns REPRESENTED characters (escapes
// decoded); `currentBytes()` stays verbatim — additive, zero-copy key
// matching unchanged.

namespace {
// Builds `int8[] buf` from a cajeta String literal inside the snippet.
constexpr const char* BYTES_HELPER =
    "    static #int8[] bytesOf(String s) {\n"
    "        int32 n = s.byteLength();\n"
    "        int8[] b = heap int8[n];\n"
    "        int32 i = 0;\n"
    "        while (i < n) {\n"
    "            b[i] = (int8) s.byteAt(i);\n"
    "            i = i + 1;\n"
    "        }\n"
    "        return #b;\n"
    "    }\n";
} // namespace

// `["a\nb\"c\\d\/e"]`: decoded = a,0A,b,22,c,5C,d,2F,e (9 bytes);
// verbatim keeps the two-byte escapes (13 bytes). Both from one token.

// `["Aé中😀"]` spelled entirely in \uXXXX escapes: BMP escapes and a
// surrogate pair decode to UTF-8: 41 | C3 A9 | E4 B8 AD | F0 9F 98 80
// (10 bytes).

// `["\b\f\r\t"]` → 08 0C 0D 09; and `currentDecodedString()` wraps the
// same bytes as a String (byteLength 4).

// Escape-free string and a NUMBER token: decoded == verbatim on both —
// the accessor is additive and changes nothing for clean input.
