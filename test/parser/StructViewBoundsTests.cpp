//
// Session 5.5 — struct view construction bounds check.
//
// The view constructor emits `count * elem_size >= sizeof(struct)`. On
// failure it routes through `__cajeta_throw`, which a try/catch in user
// code can intercept. With no surrounding catch the runtime aborts (which
// gtest can't trap from outside, so we wrap every undersize case in a
// try/catch and look for the throw value).
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.B");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

// --- Bounds-check satisfied ------------------------------------------------

TEST(StructViewBoundsTests, sufficientBufferConstructs) {
    // 16 bytes (int32[4]) is exactly enough for struct of two int32 + int64.
    auto src =
        "package test;\n"
        "@HostEndian\n"
        "public view Hdr {\n"
        "    int32 a;\n"
        "    int64 b;\n"
        "    int32 c;\n"
        "}\n"
        "public final class B {\n"
        "    public static int32 run() {\n"
        "        int32[] bytes = heap int32[4];\n"     // 16 bytes
        "        Hdr h = Hdr(bytes);\n"
        "        h.a = 1;\n"
        "        h.c = 3;\n"
        "        return h.a + h.c;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 4);
}

// --- Bounds-check fires on undersize buffer -------------------------------

TEST(StructViewBoundsTests, undersizeBufferThrows) {
    // Struct is 16 bytes (two int32 + int64). Pass int32[2] = 8 bytes —
    // should trigger the bounds check, which throws. User-land catch
    // returns 7 to signal it ran.
    auto src =
        "package test;\n"
        "@HostEndian\n"
        "public view Hdr {\n"
        "    int32 a;\n"
        "    int64 b;\n"
        "    int32 c;\n"
        "}\n"
        "public final class B {\n"
        "    public static int32 run() {\n"
        "        int32[] bytes = heap int32[2];\n"     // only 8 bytes — too small
        "        try {\n"
        "            Hdr h = Hdr(bytes);\n"           // bounds check fires here
        "            return 99;\n"
        "        } catch (Exception e) {\n"
        "            return 7;\n"
        "        }\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

TEST(StructViewBoundsTests, exactSizeBufferConstructs) {
    // Exact-size buffer (no slop). Should succeed.
    auto src =
        "package test;\n"
        "@HostEndian\n"
        "public view Duo {\n"
        "    int32 a;\n"
        "    int32 b;\n"
        "}\n"
        "public final class B {\n"
        "    public static int32 run() {\n"
        "        int32[] bytes = heap int32[2];\n"     // 8 bytes, struct is 8
        "        Duo p = Duo(bytes);\n"
        "        p.a = 11;\n"
        "        p.b = 22;\n"
        "        return p.a + p.b;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 33);
}

TEST(StructViewBoundsTests, oneByteShortStillThrows) {
    // 7 bytes available (1 byte short of 8-byte Duo struct). With int32
    // elements (4 bytes each), the closest we can get to 7 is int32[1] = 4
    // bytes — also too small. The throw still fires. We just verify any
    // undersize buffer is rejected.
    auto src =
        "package test;\n"
        "@HostEndian\n"
        "public view Duo {\n"
        "    int32 a;\n"
        "    int32 b;\n"
        "}\n"
        "public final class B {\n"
        "    public static int32 run() {\n"
        "        int32[] bytes = heap int32[1];\n"
        "        try {\n"
        "            Duo p = Duo(bytes);\n"
        "            return 0;\n"
        "        } catch (Exception e) {\n"
        "            return 1;\n"
        "        }\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}
