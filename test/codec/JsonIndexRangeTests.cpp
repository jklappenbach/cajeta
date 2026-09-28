// JsonIndex.buildRange indexes a window of a larger buffer in place, with
// positions absolute in the buffer, so a body inside a connection buffer is
// indexed without a copy.

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"

#include <cstdint>

using cajeta_test::CajetaJit;

namespace {
int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.J");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

constexpr const char* PRELUDE =
    "package test;\n"
    "import cajeta.codec.json.JsonIndex;\n"
    "import cajeta.lang.String;\n"
    "public final class J {\n"
    "    static int32 put(int8[] b, int32 at, String s) {\n"
    "        int32 i = 0;\n"
    "        while (i < s.byteLength()) { b[at + i] = s.byteAt(i); i = i + 1; }\n"
    "        return at + s.byteLength();\n"
    "    }\n";
}

TEST(JsonIndexRangeTests, rangeMatchesAWholeBufferIndexShifted) {
    std::string src = std::string(PRELUDE) +
        "    public static int32 run() {\n"
        "        String doc = \"{\\\"name\\\":\\\"ada\\\",\\\"age\\\":36,\\\"tags\\\":[1,2],\\\"x\\\\\\\"q\\\":true}\";\n"
        "        int32 n = doc.byteLength();\n"
        "        int8[] whole = heap int8[n];\n"
        "        J.put(whole, 0, doc);\n"
        "        int8[] big = heap int8[n + 60];\n"
        "        J.put(big, 0, \"POST /x HTTP/1.1\\r\\n\\r\\n{{{{\\\"\\\"\\\"\");\n"
        "        J.put(big, 37, doc);\n"
        "        int32[] a = heap int32[n + 16];\n"
        "        int32[] b = heap int32[n + 16];\n"
        "        int32 ca = JsonIndex.build(whole, (int64) n, a);\n"
        "        int32 cb = JsonIndex.buildRange(big, 37, (int64) (37 + n), b);\n"
        "        if (ca != cb) { return 1000 + cb; }\n"
        "        int32 i = 0;\n"
        "        while (i < ca) {\n"
        "            if (b[i] != a[i] + 37) { return 2000 + i; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        if (!JsonIndex.keyEq(big, (int64) b[1], \"name\")) { return 3; }\n"
        "        if (JsonIndex.decodeI64(big, (int64) b[7]) != 36) { return 4; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 0);
}

TEST(JsonIndexRangeTests, rangeStopsAtItsEnd) {
    std::string src = std::string(PRELUDE) +
        "    public static int32 run() {\n"
        "        int8[] big = heap int8[80];\n"
        "        int32 end = J.put(big, 5, \"{\\\"a\\\":1}\");\n"
        "        J.put(big, end, \"{\\\"b\\\":[2,3,4],\\\"c\\\":{}}\");\n"
        "        int32[] idx = heap int32[80];\n"
        "        int32 c = JsonIndex.buildRange(big, 5, (int64) end, idx);\n"
        "        if (c != 5) { return 100 + c; }\n"
        "        return idx[4] == end - 1 ? 0 : 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 0);
}
