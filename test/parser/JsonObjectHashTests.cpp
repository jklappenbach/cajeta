// JsonObject hashed lookup (cajeta-llama 5.2.4): at 16+ entries `get` builds
// a lazily-constructed open-addressed index and probes it instead of scanning.
// A safetensors header (thousands of tensors) and a tokenizer vocab (~100k
// keys) put every lookup through this path; the v1 linear scan made a
// parse-all-keys pass O(n²). These tests pin the hashed path's semantics
// against the documented linear-scan contract: same hits, same misses, and
// first-match-wins for duplicate keys.

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {
int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}
} // namespace

// 200 keys parsed from real JSON text, each fetched by name through the
// hashed path, plus misses. 200 is far past the 16-entry threshold, and the
// generated keys ("k0".."k199") collide freely in the probe table.
TEST(JsonObjectHashTests, hashedLookupMatchesEveryKeyAndMisses) {
    std::string doc = "{";
    for (int i = 0; i < 200; i++) {
        if (i) doc += ",";
        doc += "\\\"k" + std::to_string(i) + "\\\":" + std::to_string(i * 7);
    }
    doc += "}";
    std::string src =
        "package test;\n"
        "import cajeta.codec.json.Json;\n"
        "import cajeta.codec.json.JsonObject;\n"
        "import cajeta.codec.json.JsonValue;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        String text = \"" + doc + "\";\n"
        "        int8[] b #= text.toBytes();\n"
        "        JsonValue v #= Json.parse(b, (int64) b.count());\n"
        "        JsonObject o = v.asObject();\n"
        "        if (o.count() != 200) { return -1; }\n"
        "        int32 i = 0;\n"
        "        while (i < 200) {\n"
        "            String key = \"k\" + i;\n"
        "            JsonValue e = o.get(key);\n"
        "            if (e == null) { return -1000 - i; }\n"
        "            if (e.asInt64() != (int64) (i * 7)) { return -2000 - i; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        if (o.get(\"k200\") != null) { return -2; }\n"
        "        if (o.get(\"absent\") != null) { return -3; }\n"
        "        if (o.get(\"\") != null) { return -4; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// Duplicate keys: `put` documents that a re-put key appends an entry `get`
// never reaches. The hashed path must keep the same first-match answer.
TEST(JsonObjectHashTests, duplicateKeyKeepsFirstMatchAboveThreshold) {
    std::string src =
        "package test;\n"
        "import cajeta.codec.json.JsonObject;\n"
        "import cajeta.codec.json.JsonValue;\n"
        "public final class D {\n"
        "    static void putNum(JsonObject o, String key, int64 num) {\n"
        "        int8[] kb = key.toBytes();\n"
        "        JsonValue v = heap JsonValue();\n"
        "        v.setNumber(num);\n"
        "        o.put(#kb, key.byteLength(), #v);\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        JsonObject o = heap JsonObject();\n"
        "        D.putNum(o, \"dup\", 111);\n"
        "        int32 i = 0;\n"
        "        while (i < 30) {\n"                     // push well past HASH_MIN
        "            D.putNum(o, \"fill\" + i, (int64) i);\n"
        "            i = i + 1;\n"
        "        }\n"
        "        D.putNum(o, \"dup\", 222);\n"           // shadowed forever
        "        JsonValue v = o.get(\"dup\");\n"
        "        if (v == null) { return -1; }\n"
        "        if (v.asInt64() != 111) { return -2; }\n"
        "        if (o.get(\"fill29\").asInt64() != 29) { return -3; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// Regression pin for json-grow-element-uaf's ARRAY site: JsonArray.grow
// copied its JsonValues with plain `=` borrow stores, then displaced the old
// backing array with `#=` — whose element-drop walk freed every value the
// new array pointed at. Any array past its 8-element starting capacity read
// freed memory. 40 elements forces two grows; every element must survive.
// (JsonObject's identical site is exercised by the two tests above; Headers
// and QueryParams got the same one-line fix, covered by the net suites.)
TEST(JsonObjectHashTests, arrayGrowthKeepsElementsAlive) {
    std::string doc = "[";
    for (int i = 0; i < 40; i++) {
        if (i) doc += ",";
        doc += std::to_string(i * 3);
    }
    doc += "]";
    std::string src =
        "package test;\n"
        "import cajeta.codec.json.Json;\n"
        "import cajeta.codec.json.JsonArray;\n"
        "import cajeta.codec.json.JsonValue;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        String text = \"" + doc + "\";\n"
        "        int8[] b #= text.toBytes();\n"
        "        JsonValue v #= Json.parse(b, (int64) b.count());\n"
        "        JsonArray a = v.asArray();\n"
        "        if (a.count() != 40) { return -1; }\n"
        "        int32 i = 0;\n"
        "        while (i < 40) {\n"
        "            JsonValue e = a.get(i);\n"
        "            if (e == null) { return -1000 - i; }\n"
        "            if (e.asInt64() != (int64) (i * 3)) { return -2000 - i; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// The index is built lazily by `get` and falls behind `put`; a stale index
// must rebuild on the next `get` so late-added keys are found.
TEST(JsonObjectHashTests, putAfterGetRebuildsTheIndex) {
    std::string src =
        "package test;\n"
        "import cajeta.codec.json.JsonObject;\n"
        "import cajeta.codec.json.JsonValue;\n"
        "public final class D {\n"
        "    static void putNum(JsonObject o, String key, int64 num) {\n"
        "        int8[] kb = key.toBytes();\n"
        "        JsonValue v = heap JsonValue();\n"
        "        v.setNumber(num);\n"
        "        o.put(#kb, key.byteLength(), #v);\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        JsonObject o = heap JsonObject();\n"
        "        int32 i = 0;\n"
        "        while (i < 20) {\n"
        "            D.putNum(o, \"a\" + i, (int64) i);\n"
        "            i = i + 1;\n"
        "        }\n"
        "        if (o.get(\"a7\").asInt64() != 7) { return -1; }\n"   // builds index
        "        D.putNum(o, \"late\", 555);\n"                        // index now stale
        "        JsonValue v = o.get(\"late\");\n"
        "        if (v == null) { return -2; }\n"                      // rebuild found it
        "        if (v.asInt64() != 555) { return -3; }\n"
        "        if (o.get(\"a0\").asInt64() != 0) { return -4; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}
