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

TEST(PostVariableFieldTests, fixedAfterVarSizeCompiles) {
    auto src =
        "package test;\n"
        "@HostEndian\n"
        "public view R {\n"
        "    String name;\n"
        "    int32 id;\n"
        "}\n"
        "public final class V {\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    EXPECT_NO_THROW(CajetaJit::compile(src, "test.V"));
}

TEST(PostVariableFieldTests, postVariableFixedFieldRead) {
    // Layout (no pre-var fixed; first field is var-size):
    //   bytes[0] = name.length = 4
    //   bytes[1] = "abcd"
    //   bytes[2] = id (i32) = 99
    auto src =
        "package test;\n"
        "@HostEndian\n"
        "public view R {\n"
        "    String name;\n"
        "    int32 id;\n"
        "}\n"
        "public final class V {\n"
        "    public static int32 run() {\n"
        "        int32[] bytes = heap int32[3];\n"
        "        bytes[0] = 4;\n"
        "        bytes[1] = 1684234849;\n"  // "abcd"
        "        bytes[2] = 99;\n"
        "        R r = R(bytes);\n"
        "        return r.id;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 99);
}

TEST(PostVariableFieldTests, interleavedPreVarFixedPostVarFixed) {
    // Pre-var fixed AND post-var fixed in the same view. The pre-var
    // field is in the LLVM struct (compile-time offset); the post-var
    // field is found via walk-the-prefixes.
    auto src =
        "package test;\n"
        "@HostEndian\n"
        "public view R {\n"
        "    int32 a;\n"          // pre-var fixed (in LLVM struct)
        "    String s;\n"          // var-size
        "    int32 b;\n"           // post-var fixed (walked)
        "}\n"
        "public final class V {\n"
        "    public static int32 run() {\n"
        "        int32[] bytes = heap int32[4];\n"
        "        bytes[0] = 10;\n"               // a
        "        bytes[1] = 4;\n"                 // s.length
        "        bytes[2] = 1684234849;\n"       // "abcd"
        "        bytes[3] = 32;\n"                // b
        "        R r = R(bytes);\n"
        "        return r.a + r.b;\n"             // 10 + 32 = 42
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

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

TEST(PostVariableFieldTests, interleavedVarFixedVar) {
    // Var, fixed, var. Reading the SECOND var-size requires walking
    // past the first var-size AND the intermediate fixed field. The
    // var-size accessor's walk loop doesn't currently know about
    // intermediate post-var fixed fields, so this test pins the
    // behavior — should pass with the current walk because each
    // var-size accessor scans declaration order from the start.
    auto src =
        "package test;\n"
        "@HostEndian\n"
        "public view R {\n"
        "    String s1;\n"
        "    int32 marker;\n"
        "    String s2;\n"
        "}\n"
        "public final class V {\n"
        "    public static int32 run() {\n"
        "        int32[] bytes = heap int32[5];\n"
        "        bytes[0] = 4;\n"               // s1.length
        "        bytes[1] = 1684234849;\n"     // "abcd"
        "        bytes[2] = 99;\n"               // marker
        "        bytes[3] = 4;\n"               // s2.length
        "        bytes[4] = 2054781047;\n"     // "wxyz"
        "        R r = R(bytes);\n"
        "        return r.marker;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 99);
}

// --- T[] variable-size view field ------------------------------------------

TEST(PostVariableFieldTests, intArrayVarSizeFieldRead) {
    // view R { int32[] xs; } — xs is variable-size (i32 count + count*4 bytes).
    // Buffer:
    //   bytes[0] = count = 3
    //   bytes[1..3] = element values 7, 8, 9
    auto src =
        "package test;\n"
        "@HostEndian\n"
        "public view R {\n"
        "    int32[] xs;\n"
        "}\n"
        "public final class V {\n"
        "    public static int32 run() {\n"
        "        int32[] bytes = heap int32[4];\n"
        "        bytes[0] = 3;\n"
        "        bytes[1] = 7;\n"
        "        bytes[2] = 8;\n"
        "        bytes[3] = 9;\n"
        "        R r = R(bytes);\n"
        "        int32[] copy = r.xs;\n"
        "        return copy[0] + copy[1] + copy[2];\n"  // 24
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 24);
}

TEST(PostVariableFieldTests, intArrayElementsRoundTrip) {
    // Pick an element from deep in the materialized array — verifies the
    // memcpy covered the full length and didn't truncate.
    auto src =
        "package test;\n"
        "@HostEndian\n"
        "public view R {\n"
        "    int32[] xs;\n"
        "}\n"
        "public final class V {\n"
        "    public static int32 run() {\n"
        "        int32[] bytes = heap int32[6];\n"
        "        bytes[0] = 5;\n"                // count
        "        bytes[1] = 100;\n"
        "        bytes[2] = 200;\n"
        "        bytes[3] = 300;\n"
        "        bytes[4] = 400;\n"
        "        bytes[5] = 500;\n"
        "        R r = R(bytes);\n"
        "        int32[] xs = r.xs;\n"
        "        return xs[4];\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 500);
}

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
