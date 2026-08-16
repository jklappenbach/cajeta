// Json factory entry-point tests (Tier 3 sugar).
// Verifies Json.parse / Json.toBytes wrap JsonReader/JsonWriter
// correctly and round-trip through the JsonValue tree.

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
    "import cajeta.codec.json.Json;\n"
    "import cajeta.codec.json.JsonValue;\n"
    "import cajeta.codec.json.JsonArray;\n";
} // namespace

// Json.parse over `42` → JsonValue kind=NUMBER, asInt64==42.
TEST(JsonFactoryTests, parseNumber) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int8[] buf = [(int8) 52, (int8) 50];\n"
        "        JsonValue v #= Json.parse(buf, (int64) 2);\n"
        "        return v.asInt32();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// Json.parse over `[10,20,30]` → JsonArray of three NUMBER values.
TEST(JsonFactoryTests, parseArraySum) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        // [10,20,30]
        "        int8[] buf = [(int8) 91, (int8) 49, (int8) 48, (int8) 44,\n"
        "                      (int8) 50, (int8) 48, (int8) 44, (int8) 51,\n"
        "                      (int8) 48, (int8) 93];\n"
        "        JsonValue v #= Json.parse(buf, (int64) 10);\n"
        "        JsonArray a = v.asArray();\n"
        "        int32 sum = 0;\n"
        "        int32 i = 0;\n"
        "        while (i < a.count()) {\n"
        "            sum = sum + a.get(i).asInt32();\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return sum;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 60);
}

// Json.toBytes over a programmatically-built JsonValue tree, then
// re-parse via Json.parse and verify the round-trip.
TEST(JsonFactoryTests, toBytesAndReparseSum) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        JsonArray a = heap JsonArray();\n"
        "        JsonValue e0 = heap JsonValue(); e0.setNumber((int64) 5);\n"
        "        JsonValue e1 = heap JsonValue(); e1.setNumber((int64) 9);\n"
        "        a.add(#e0);\n"
        "        a.add(#e1);\n"
        "        JsonValue v = heap JsonValue();\n"
        "        v.setArray(#a);\n"
        "        int8[] bytes #= Json.toBytes(v);\n"
        // Find the bytes length by counting until we hit the closing
        // ']' (byte 93). Cheap because the input is bounded.
        "        int32 len = 0;\n"
        "        while (bytes[len] != (int8) 93) {\n"
        "            len = len + 1;\n"
        "        }\n"
        "        len = len + 1;\n"
        "        JsonValue parsed #= Json.parse(bytes, (int64) len);\n"
        "        JsonArray got = parsed.asArray();\n"
        "        return got.get(0).asInt32() + got.get(1).asInt32();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 14);
}

// Json.parse over `null` → JsonValue with kind=NULL (isNull true).
TEST(JsonFactoryTests, parseNullLiteral) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        // 'n','u','l','l'
        "        int8[] buf = [(int8) 110, (int8) 117, (int8) 108, (int8) 108];\n"
        "        JsonValue v #= Json.parse(buf, (int64) 4);\n"
        "        if (v.isNull()) { return 1; }\n"
        "        return -1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}
