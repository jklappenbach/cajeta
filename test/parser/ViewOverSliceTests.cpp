//
// A view constructed over a Slice window: `V(buf[a:b])` overlays the window,
// so fields read from the window's first byte and every size and prefix
// check runs against the window's length, not the root's.
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

const char* kViews =
    "package test;\n"
    "import cajeta.lang.Slice;\n"
    "@LittleEndian\n"
    "public view H {\n"
    "    int16 a;\n"
    "    int32 b;\n"
    "}\n"
    "@LittleEndian\n"
    "public view N {\n"
    "    int8   tag;\n"
    "    String name;\n"
    "}\n";

} // namespace

TEST(ViewOverSliceTests, fixedViewReadsFromTheWindowStart) {
    auto src = std::string(kViews) +
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int8[] buf = heap int8[16];\n"
        "        buf[5] = (int8) 3;\n"
        "        buf[7] = (int8) 40;\n"
        "        H h = H(buf[5:11]);\n"
        "        return (int32) h.a * 100 + h.b;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 340);
}

TEST(ViewOverSliceTests, viewOverASliceLocal) {
    auto src = std::string(kViews) +
        "public final class S {\n"
        "    static int32 read(Slice<int8> w) {\n"
        "        H h = H(w);\n"
        "        return h.b;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int8[] buf = heap int8[32];\n"
        "        buf[12] = (int8) 9;\n"
        "        Slice<int8> w = buf[10:20];\n"
        "        return S.read(w) + S.read(buf[10:16]);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 18);
}

TEST(ViewOverSliceTests, windowShorterThanTheLayoutThrows) {
    auto src = std::string(kViews) +
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int8[] buf = heap int8[64];\n"
        "        try {\n"
        "            H h = H(buf[0:5]);\n"
        "            return 99;\n"
        "        } catch (Exception e) {\n"
        "            return 7;\n"
        "        }\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

TEST(ViewOverSliceTests, prefixIsCheckedAgainstTheWindow) {
    auto src = std::string(kViews) +
        "public final class S {\n"
        "    static int32 build(int8[] buf) {\n"
        "        buf[2] = (int8) 1;\n"
        "        buf[3] = (int8) 4;\n"
        "        buf[7] = (int8) 97; buf[8] = (int8) 98; buf[9] = (int8) 99; buf[10] = (int8) 100;\n"
        "        return 0;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int8[] buf = heap int8[64];\n"
        "        S.build(buf);\n"
        "        N whole = N(buf[2:11]);\n"
        "        String got #= whole.name;\n"
        "        int32 ok = got.byteLength() == 4 && got.byteAt(3) == (int8) 100 ? 1 : 0;\n"
        "        try {\n"
        "            N cut = N(buf[2:10]);\n"
        "            return 99;\n"
        "        } catch (Exception e) {\n"
        "            return ok * 10 + 7;\n"
        "        }\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 17);
}

TEST(ViewOverSliceTests, writesLandInTheRootAtTheOffset) {
    auto src = std::string(kViews) +
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int8[] buf = heap int8[16];\n"
        "        H h = H(buf[8:14]);\n"
        "        h.b = 258;\n"
        "        return (int32) buf[10] * 10 + (int32) buf[11];\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 21);
}

TEST(ViewOverSliceTests, wideElementWindowCountsBytes) {
    auto src = std::string(kViews) +
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32[] words = heap int32[8];\n"
        "        words[3] = 5 * 65536;\n"
        "        H h = H(words[3:5]);\n"
        "        return h.b;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 5);
}

TEST(ViewOverSliceTests, owningFormOverASliceIsRejected) {
    auto src = std::string(kViews) +
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int8[] buf = heap int8[16];\n"
        "        Slice<int8> w = buf[0:8];\n"
        "        H h = H(#w);\n"
        "        return h.b;\n"
        "    }\n"
        "}\n";
    EXPECT_ANY_THROW(CajetaJit::compile(src, "test.S"));
}

TEST(ViewOverSliceTests, viewOverASliceOfALocalCannotEscape) {
    auto src = std::string(kViews) +
        "public final class S {\n"
        "    public static H escape() {\n"
        "        int8[] buf = heap int8[16];\n"
        "        H h = H(buf[2:10]);\n"
        "        return h;\n"
        "    }\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    EXPECT_ANY_THROW(CajetaJit::compile(src, "test.S"));
}

namespace {

// Reads `v.name` through `form` 1000 times after a warm-up; returns the
// String's length when the live count held, -1 when it moved.
std::string readLoop(const std::string& body) {
    return std::string(kViews) +
        "public final class S {\n"
        "    static cajeta.collection.ArrayList<String> kept = heap cajeta.collection.ArrayList<String>();\n"
        "    static int32 take(String s) { return s.byteLength(); }\n"
        "    static int32 keep(#String s) { return s.byteLength(); }\n"
        "    static #String pass(N v) { return v.name; }\n"
        "    static int32 read(N v) {\n" + body +
        "    }\n"
        "    public static int32 run() {\n"
        "        int8[] buf = heap int8[48];\n"
        "        buf[1] = (int8) 40;\n"
        "        int32 i = 0;\n"
        "        while (i < 40) { buf[5 + i] = (int8) 97; i = i + 1; }\n"
        "        N whole = N(buf[0:45]);\n"
        "        int32 first = S.read(whole);\n"
        "        int64 before = Cajeta.liveCount();\n"
        "        int32 n = 0;\n"
        "        while (n < 1000) { first = S.read(whole); n = n + 1; }\n"
        "        int64 grew = Cajeta.liveCount() - before;\n"
        "        return grew == 0 ? first : -1;\n"
        "    }\n"
        "}\n";
}

} // namespace

TEST(ViewOverSliceTests, viewStringFieldReadDropsInEveryPosition) {
    EXPECT_EQ(runI32(readLoop("        String got #= v.name;\n        S.kept.add(#got);\n        return 40;\n")), -1);
    EXPECT_EQ(runI32(readLoop("        String got #= v.name;\n        return got.byteLength();\n")), 40);
    EXPECT_EQ(runI32(readLoop("        String got = v.name;\n        return got.byteLength();\n")), 40);
    EXPECT_EQ(runI32(readLoop("        return S.take(v.name);\n")), 40);
    EXPECT_EQ(runI32(readLoop("        return v.name.byteLength();\n")), 40);
    EXPECT_EQ(runI32(readLoop("        String c = \"<\" + v.name;\n        return c.byteLength() - 1;\n")), 40);
    EXPECT_EQ(runI32(readLoop("        String p #= S.pass(v);\n        return p.byteLength();\n")), 40);
}

// DISABLED: a fresh String passed to a `#String` formal is never freed. The callee arms no
// drop entry for a String formal, and arming one breaks the stdlib's `#String` forwarding.
TEST(ViewOverSliceTests, freshStringIntoOwnedFormalIsFreed) {
    EXPECT_EQ(runI32(readLoop("        return S.keep(v.name);\n")), 40);
    EXPECT_EQ(runI32(readLoop("        return S.keep(\"<\" + v.name) - 1;\n")), 40);
}
