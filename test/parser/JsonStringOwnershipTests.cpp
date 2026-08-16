// JsonValue.asString / JsonObject.keys ownership (the cajeta-http 1.4e
// find).
//
// The (#int8[], int32) String ctor CONSUMES its buffer: its @Native core
// (__cajeta_string_adopt) inline-copies AND FREES short inputs (<= the
// inline cap) and ADOPTS long ones as the String's owned root. @Native
// bodies never see the runtime title flag, so a call site that passes a
// borrow still loses the buffer.
//
// JsonValue.asString() and JsonObject.keys() used to feed the DOM's own
// strBytes / stored key buffers to that ctor. First read: the DOM's
// buffer was freed (short) or aliased (long); the next allocation
// recycled it and every later read returned garbage — reads are what
// freed the DOM. The fix: both sites wrap a fresh copy.
//
// The probe pattern: read, churn the allocator so any freed chunk is
// recycled, read again, compare. Stale-but-freed memory passes a single
// read; it cannot pass this.

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
    "import cajeta.codec.json.JsonObject;\n"
    "import cajeta.collection.ArrayList;\n"
    "import cajeta.lang.String;\n";

constexpr const char* CHURN =
    "    static void churn() {\n"
    "        int32 i = 0;\n"
    "        while (i < 256) {\n"
    "            int8[] junk = heap int8[8];\n"
    "            int32 j = 0;\n"
    "            while (j < 8) { junk[j] = (int8) 0x2A; j = j + 1; }\n"
    "            i = i + 1;\n"
    "        }\n"
    "        int32 k = 0;\n"
    "        while (k < 64) {\n"
    "            int8[] big = heap int8[24];\n"
    "            int32 j = 0;\n"
    "            while (j < 24) { big[j] = (int8) 0x2A; j = j + 1; }\n"
    "            k = k + 1;\n"
    "        }\n"
    "    }\n";
} // namespace

// A short (inline-cap) string read twice with churn between. Before the
// fix, the FIRST asString freed strBytes; the churn recycled the chunk
// and the second read returned junk.
TEST(JsonStringOwnershipTests, asStringDoesNotConsumeShortStrBytes) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n" + CHURN +
        "    public static int32 run() {\n"
        "        JsonValue v = heap JsonValue();\n"
        "        String s = \"cajeta\";\n"
        "        int8[] b #= s.toBytes();\n"
        "        v.setStringOwned(#b, 6);\n"
        "        D.churn();\n"
        "        String a #= v.asString();\n"
        "        D.churn();\n"
        "        String c #= v.asString();\n"
        "        if (!a.equals(\"cajeta\")) { return -1; }\n"
        "        if (!c.equals(\"cajeta\")) { return -2; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// A LONG (> inline cap) string: the consuming ctor used to ADOPT the
// DOM's buffer — an alias whose drop double-frees. Two reads + drops of
// the materialized Strings must leave the DOM readable.
TEST(JsonStringOwnershipTests, asStringDoesNotAdoptLongStrBytes) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n" + CHURN +
        "    public static int32 run() {\n"
        "        JsonValue v = heap JsonValue();\n"
        "        String s = \"the quick brown fox\";\n"
        "        int8[] b #= s.toBytes();\n"
        "        v.setStringOwned(#b, s.byteLength());\n"
        "        scope {\n"
        "            String a #= v.asString();\n"
        "            if (!a.equals(\"the quick brown fox\")) { return -1; }\n"
        "        }\n"
        "        D.churn();\n"
        "        String c #= v.asString();\n"
        "        if (!c.equals(\"the quick brown fox\")) { return -2; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// JsonObject.keys() materializes each stored key — it must not consume
// the object's key buffers: keyAt() must still be usable afterwards.
TEST(JsonStringOwnershipTests, keysDoesNotConsumeStoredKeyBuffers) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n" + CHURN +
        "    public static int32 run() {\n"
        "        JsonObject o = heap JsonObject();\n"
        "        String kn = \"name\";\n"
        "        int8[] k #= kn.toBytes();\n"
        "        JsonValue sv = heap JsonValue();\n"
        "        String vn = \"cajeta\";\n"
        "        int8[] b #= vn.toBytes();\n"
        "        sv.setStringOwned(#b, 6);\n"
        "        o.put(#k, 4, #sv);\n"
        "        scope {\n"
        "            ArrayList<String> ks #= o.keys();\n"
        "            if (ks.count() != 1) { return -1; }\n"
        "        }\n"
        "        D.churn();\n"
        "        if (!o.containsKey(\"name\")) { return -2; }\n"
        "        JsonValue g = o.get(\"name\");\n"
        "        if (g == null) { return -3; }\n"
        "        String got #= g.asString();\n"
        "        if (!got.equals(\"cajeta\")) { return -4; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// End-to-end over the Tier-3 parse: read a parsed field's string twice
// with churn between — the getJson consumption pattern.
TEST(JsonStringOwnershipTests, parsedDomSurvivesRepeatedStringReads) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n" + CHURN +
        "    public static int32 run() {\n"
        "        String s = \"{\\\"name\\\":\\\"cajeta\\\"}\";\n"
        "        int8[] buf #= s.toBytes();\n"
        "        JsonValue v #= Json.parse(buf, (int64) s.byteLength());\n"
        "        D.churn();\n"
        "        String a #= v.asObject().get(\"name\").asString();\n"
        "        D.churn();\n"
        "        String c #= v.asObject().get(\"name\").asString();\n"
        "        if (!a.equals(\"cajeta\")) { return -1; }\n"
        "        if (!c.equals(\"cajeta\")) { return -2; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}
