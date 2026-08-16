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
