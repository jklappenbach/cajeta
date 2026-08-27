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

// Empty array emit: writer produces `[]`.

// Array of three numbers: [1,2,3] (7 bytes). Verify size + the
// digits at the expected offsets.

// Larger integer formatting: -1234567890 → "-1234567890" (11 bytes).

// Booleans + null: [true,false,null] (17 bytes).

// Object with single string-valued key: {"a":"b"} — 9 bytes
// (`{`, `"`, `a`, `"`, `:`, `"`, `b`, `"`, `}`).

// Round-trip: writer emits `[1,2,3]` and reader walks it back.

// currentNumberAsInt64 negative parse.

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

// Control characters in strings MUST be escaped (RFC 8259 §7): a chunk
// of streamed model text containing a newline previously passed through
// writeStringRaw verbatim, producing invalid JSON and splitting a JSONL
// line in two (found by cabra's protocol round-trip test, 2026-08-27).
// The emitted buffer must contain NO raw byte < 0x20, and the value must
// round-trip through the reader unchanged.
TEST(JsonWriterTests, controlCharactersAreEscapedAndRoundTrip) {
    auto src = std::string(
        "package test;\n"
        "import cajeta.codec.json.Json;\n"
        "import cajeta.codec.json.JsonObject;\n"
        "import cajeta.codec.json.JsonValue;\n"
        "import cajeta.codec.json.JsonWriter;\n"
        "import cajeta.lang.String;\n") +
        "public final class D {\n"
        "    public static int32 run() {\n"
        // "a\nb\x01c\td" — newline, an unnamed C0 control, a tab.
        "        int8[] raw = [\n"
        "            (int8) 97, (int8) 10, (int8) 98, (int8) 1,\n"
        "            (int8) 99, (int8) 9, (int8) 100\n"
        "        ];\n"
        "        int8[] raw2 = heap int8[7];\n"
        "        int32 i = 0;\n"
        "        while (i < 7) { raw2[i] = raw[i]; i = i + 1; }\n"
        "        String s = heap String(#raw2, 7);\n"
        "        JsonWriter w = heap JsonWriter();\n"
        "        w.beginObject().key(\"k\").writeString(s).endObject();\n"
        "        int8[] out #= w.toBytes();\n"
        "        int32 bad = 0;\n"
        "        i = 0;\n"
        "        while (i < (int32) out.count()) {\n"
        "            if (((int32) out[i] & 255) < 32) { bad = bad + 1; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        JsonValue v #= Json.parse(out, (int64) out.count());\n"
        "        String got #= v.object().get(\"k\").asString();\n"
        "        int32 eq = 0;\n"
        "        if (!got.equals(s)) { eq = 100; }\n"
        "        return bad + eq;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(0, runI32(src));
}
