//
// S5b — fixed-size fields after a variable-size field; T[] view fields.
//
// S5 lifted the "single trailing var-size" limit but kept "fixed-after-var
// is an error." S5b drops that restriction too: fixed fields after a
// variable-size field use the same walk-the-prefixes scheme as the
// var-size accessors, advancing past intermediate var-size data at
// runtime to find their byte offset.
//
// Also covers T[] as a variable-size view field — runtime helper
// __cajeta_array_view_to_owned allocates a fresh array header + data
// block and memcpys the wire bytes into it. Element size is emitted as
// a compile-time constant from the field's CajetaArray element type.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.S");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

// --- Post-variable fixed field reads ---------------------------------------




TEST(PostVariableFieldTests, multiplePostVariableFixedFields) {
    // Two post-var fixed fields. The second one's walk has to skip
    // both the var-size data AND the first post-var fixed.
    auto src =
        "package test;\n"
        "@HostEndian\n"
        "public view R {\n"
        "    String s;\n"
        "    int32 a;\n"
        "    int32 b;\n"
        "}\n"
        "public final class V {\n"
        "    public static int32 run() {\n"
        "        int32[] bytes = heap int32[4];\n"
        "        bytes[0] = 4;\n"               // s.length
        "        bytes[1] = 1684234849;\n"     // "abcd"
        "        bytes[2] = 7;\n"                // a
        "        bytes[3] = 35;\n"               // b
        "        R r = R(bytes);\n"
        "        return r.a + r.b;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}


// --- T[] variable-size view field ------------------------------------------



TEST(PostVariableFieldTests, intArrayCountFromViewMatches) {
    // Materialized array's count() returns the right element count.
    // count() is the structural accessor on T[] — matches Collection.
    auto src =
        "package test;\n"
        "@HostEndian\n"
        "public view R {\n"
        "    int32[] xs;\n"
        "}\n"
        "public final class V {\n"
        "    public static int32 run() {\n"
        "        int32[] bytes = heap int32[6];\n"
        "        bytes[0] = 5;\n"
        "        bytes[1] = 100;\n"
        "        bytes[2] = 200;\n"
        "        bytes[3] = 300;\n"
        "        bytes[4] = 400;\n"
        "        bytes[5] = 500;\n"
        "        R r = R(bytes);\n"
        "        int32[] xs = r.xs;\n"
        "        return (int32) xs.count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 5);
}
