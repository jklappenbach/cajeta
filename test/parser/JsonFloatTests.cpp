// Unit 2 (benchmark-fidelity-plan) — v1 JSON DOM float parsing. The DOM tree
// (JsonReader.readValue -> JsonValue) previously truncated/rejected non-integer
// numbers; these tests pin float parse, integer preservation, round-trip through
// JsonWriter, and a float-bearing object (the twitter/canada shape that made the
// json-dom/serialize/roundtrip benches skip).
#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"

#include <cstdint>

using cajeta_test::CajetaJit;

namespace {
int32_t runI32(const std::string& body) {
    std::string src =
        "package test;\n"
        "import cajeta.codec.json.JsonReader;\n"
        "import cajeta.codec.json.JsonValue;\n"
        "import cajeta.codec.json.JsonWriter;\n"
        "import cajeta.codec.json.JsonObject;\n"
        "public final class D {\n"
        "    public static int32 run() {\n" + body +
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}
// Parse `lit` to a JsonValue tree and return run-body that leaves it in `v`.
std::string parseTo(const char* lit) {
    return std::string(
        "        String s = \"") + lit + "\";\n"
        "        int8[] sb = s.toBytes();\n"
        "        JsonReader r = heap JsonReader(sb, (int64) s.byteLength());\n"
        "        JsonValue v = r.readValue();\n";
}
} // namespace

// 2.a.1 — fractional number parses as a float (kind NUMBER, isFloat, value).
TEST(JsonFloat, parsePi) {
    EXPECT_EQ(runI32(parseTo("3.14") +
        "        if (v.kind() != JsonValue.NUMBER) { return -1; }\n"
        "        if (!v.isFloat()) { return -2; }\n"
        "        return (int32) (v.asFloat64() * 100.0);\n"), 314);
}




// 2.a.1 — integers keep the integer path (isFloat false, exact int64).
TEST(JsonFloat, integerStaysInteger) {
    EXPECT_EQ(runI32(parseTo("42") +
        "        if (v.isFloat()) { return -2; }\n"
        "        return (int32) v.asInt64();\n"), 42);
}

// 2.a.1 — round-trip: parse a float, re-emit via JsonWriter, re-parse, value holds.
TEST(JsonFloat, roundTripThroughWriter) {
    EXPECT_EQ(runI32(parseTo("3.14") +
        "        JsonWriter w = heap JsonWriter();\n"
        "        w.writeValue(v);\n"
        "        int8[] out = w.toBytes();\n"
        "        JsonReader r2 = heap JsonReader(out, (int64) out.count());\n"
        "        JsonValue v2 = r2.readValue();\n"
        "        if (!v2.isFloat()) { return -2; }\n"
        "        return (int32) (v2.asFloat64() * 100.0);\n"), 314);
}

// 2.a.2 — a float-bearing object (twitter/canada shape) parses without throwing.
TEST(JsonFloat, floatObjectParses) {
    EXPECT_EQ(runI32(parseTo("{\\\"x\\\":1.5,\\\"y\\\":-2.25}") +
        "        if (v.kind() != JsonValue.OBJECT) { return -1; }\n"
        "        JsonObject o = v.asObject();\n"
        "        JsonValue x = o.get(\"x\");\n"
        "        return (int32) (x.asFloat64() * 100.0);\n"), 150);
}
