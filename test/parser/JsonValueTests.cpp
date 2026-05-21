// JsonValue Phase 3 tests. Verify the tagged-union value-tree
// works for primitive kinds + simple array / object construction.
// Reader.readValue() and Writer.writeValue() round-trips covered
// once those land.

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
    "import cajeta.codec.json.JsonValue;\n"
    "import cajeta.codec.json.JsonArray;\n"
    "import cajeta.codec.json.JsonObject;\n";
} // namespace

// JsonValue defaults to NULL kind.
TEST(JsonValueTests, defaultsToNull) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        JsonValue v = heap JsonValue();\n"
        "        if (v.isNull()) { return v.kind() + 1; }\n"
        "        return -1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);  // kind=0, isNull true → 0+1
}

// Boolean kind: setBoolean(true) then asBoolean returns true.
TEST(JsonValueTests, booleanRoundTrip) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        JsonValue v = heap JsonValue();\n"
        "        v.setBoolean(true);\n"
        "        if (v.kind() != 1) { return -1; }\n"
        "        if (!v.asBoolean()) { return -2; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// Number round-trip via setNumber/asInt64.
TEST(JsonValueTests, numberRoundTrip) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        JsonValue v = heap JsonValue();\n"
        "        v.setNumber((int64) 12345);\n"
        "        if (v.kind() != 2) { return -1; }\n"
        "        return v.asInt32();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 12345);
}

// JsonArray: add three numbers, retrieve by index, sum them.
TEST(JsonValueTests, arrayAddAndGet) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        JsonArray a = heap JsonArray();\n"
        "        JsonValue v0 = heap JsonValue(); v0.setNumber((int64) 10);\n"
        "        JsonValue v1 = heap JsonValue(); v1.setNumber((int64) 20);\n"
        "        JsonValue v2 = heap JsonValue(); v2.setNumber((int64) 30);\n"
        "        a.add(v0);\n"
        "        a.add(v1);\n"
        "        a.add(v2);\n"
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

// JsonObject: put + linear-scan get returns the right value.
TEST(JsonValueTests, objectPutAndGet) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        JsonObject o = heap JsonObject();\n"
        "        int8[] k1 = {(int8) 97, (int8) 98};\n"   // \"ab\"
        "        int8[] k2 = {(int8) 99, (int8) 100};\n"  // \"cd\"
        "        JsonValue v1 = heap JsonValue(); v1.setNumber((int64) 7);\n"
        "        JsonValue v2 = heap JsonValue(); v2.setNumber((int64) 11);\n"
        "        o.put(k1, 2, v1);\n"
        "        o.put(k2, 2, v2);\n"
        "        int8[] q = {(int8) 99, (int8) 100};\n"   // also \"cd\"\n"
        "        JsonValue got = o.get(q, 2);\n"
        "        return got.asInt32();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 11);
}

// JsonObject: get for a missing key returns null.
TEST(JsonValueTests, objectGetMissingReturnsNull) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        JsonObject o = heap JsonObject();\n"
        "        int8[] miss = {(int8) 120};\n"
        "        JsonValue got = o.get(miss, 1);\n"
        "        if (got == null) { return 42; }\n"
        "        return -1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// --- readValue() + writeValue() round-trips ---------------------

// Reader.readValue() over `42` → JsonValue kind=NUMBER, asInt64==42.
TEST(JsonValueTests, readValueParsesNumber) {
    auto src = std::string(PRELUDE) +
        "import cajeta.codec.json.JsonReader;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int8[] buf = {(int8) 52, (int8) 50};\n"   // \"42\"
        "        JsonReader r = heap JsonReader(buf, (int64) 2);\n"
        "        JsonValue v = r.readValue();\n"
        "        if (v.kind() != 2) { return -1; }\n"
        "        return v.asInt32();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// readValue over `[]` → empty JsonArray.
TEST(JsonValueTests, readValueParsesEmptyArray) {
    auto src = std::string(PRELUDE) +
        "import cajeta.codec.json.JsonReader;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        // []
        "        int8[] buf = {(int8) 91, (int8) 93};\n"
        "        JsonReader r = heap JsonReader(buf, (int64) 2);\n"
        "        JsonValue v = r.readValue();\n"
        "        if (v.kind() != 4) { return -1; }\n"
        "        return v.asArray().count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 0);
}

// readValue over `[42]` → array with one NUMBER value.
TEST(JsonValueTests, readValueParsesSingletonArray) {
    auto src = std::string(PRELUDE) +
        "import cajeta.codec.json.JsonReader;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        // [42]
        "        int8[] buf = {(int8) 91, (int8) 52, (int8) 50, (int8) 93};\n"
        "        JsonReader r = heap JsonReader(buf, (int64) 4);\n"
        "        JsonValue v = r.readValue();\n"
        "        JsonArray a = v.asArray();\n"
        "        return a.get(0).asInt32();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// readValue over `[1,2,3]` → JsonArray of three NUMBER values.
TEST(JsonValueTests, readValueParsesArray) {
    auto src = std::string(PRELUDE) +
        "import cajeta.codec.json.JsonReader;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        // [1,2,3]
        "        int8[] buf = {(int8) 91, (int8) 49, (int8) 44,\n"
        "                      (int8) 50, (int8) 44, (int8) 51, (int8) 93};\n"
        "        JsonReader r = heap JsonReader(buf, (int64) 7);\n"
        "        JsonValue v = r.readValue();\n"
        "        if (v.kind() != 4) { return -1; }\n"
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
    EXPECT_EQ(runI32(src), 6);
}

// readValue over `{\"a\":7}` → JsonObject with one entry.
TEST(JsonValueTests, readValueParsesObject) {
    auto src = std::string(PRELUDE) +
        "import cajeta.codec.json.JsonReader;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        // {\"a\":7}
        "        int8[] buf = {(int8) 123, (int8) 34, (int8) 97, (int8) 34,\n"
        "                      (int8) 58, (int8) 55, (int8) 125};\n"
        "        JsonReader r = heap JsonReader(buf, (int64) 7);\n"
        "        JsonValue v = r.readValue();\n"
        "        if (v.kind() != 5) { return -1; }\n"
        "        JsonObject o = v.asObject();\n"
        "        if (o.count() != 1) { return -2; }\n"
        "        int8[] q = {(int8) 97};\n"
        "        JsonValue got = o.get(q, 1);\n"
        "        return got.asInt32();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

// Full round-trip: parse → write → re-parse the written bytes
// → verify the value tree round-tripped.
TEST(JsonValueTests, roundTripArrayThroughValueTree) {
    auto src = std::string(PRELUDE) +
        "import cajeta.codec.json.JsonReader;\n"
        "import cajeta.codec.json.JsonWriter;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        // [10,20,30]
        "        int8[] in = {(int8) 91, (int8) 49, (int8) 48, (int8) 44,\n"
        "                     (int8) 50, (int8) 48, (int8) 44, (int8) 51,\n"
        "                     (int8) 48, (int8) 93};\n"
        "        JsonReader r1 = heap JsonReader(in, (int64) 10);\n"
        "        JsonValue v = r1.readValue();\n"
        "        JsonWriter w = heap JsonWriter();\n"
        "        w.writeValue(v);\n"
        "        int32 sz = w.size();\n"
        "        int8[] out = w.toBytes();\n"
        "        JsonReader r2 = heap JsonReader(out, (int64) sz);\n"
        "        JsonValue v2 = r2.readValue();\n"
        "        JsonArray a = v2.asArray();\n"
        "        int32 sum = 0;\n"
        "        int32 i = 0;\n"
        "        while (i < a.count()) {\n"
        "            sum = sum + a.get(i).asInt32();\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return sum;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 60);  // 10 + 20 + 30
}

// JsonValue holding an array (kind=4 ARRAY).
TEST(JsonValueTests, arrayValueRoundTrip) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        JsonArray a = heap JsonArray();\n"
        "        JsonValue elem = heap JsonValue(); elem.setNumber((int64) 99);\n"
        "        a.add(elem);\n"
        "        JsonValue v = heap JsonValue();\n"
        "        v.setArray(a);\n"
        "        if (v.kind() != 4) { return -1; }\n"
        "        return v.asArray().get(0).asInt32();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 99);
}
