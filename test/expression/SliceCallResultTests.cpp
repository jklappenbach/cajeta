// A Slice returned by a call is used directly: indexed and asked its count
// without binding it to a local first.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.U");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

const char* kBox =
    "package test;\n"
    "import cajeta.lang.Slice;\n"
    "public final class Box {\n"
    "    int8[] b;\n"
    "    public Box() { this.b = heap int8[8]; this.b[3] = (int8) 7; }\n"
    "    public Slice<int8> win() { return this.b[2:6]; }\n"
    "}\n";

} // namespace

TEST(SliceCallResultTests, indexACallResultDirectly) {
    auto src = std::string(kBox) +
        "public final class U {\n"
        "    public static int32 run() {\n"
        "        Box x = heap Box();\n"
        "        Slice<int8> w = x.win();\n"
        "        return (int32) x.win()[1] * 10 + (int32) w[1];\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 77);
}

TEST(SliceCallResultTests, countACallResultDirectly) {
    auto src = std::string(kBox) +
        "public final class U {\n"
        "    public static int32 run() {\n"
        "        Box x = heap Box();\n"
        "        return (int32) x.win().count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 4);
}

// An owned String produced from an anonymous owned receiver frees the receiver.
TEST(SliceCallResultTests, ownedStringFromATemporaryReceiverFreesIt) {
    auto src =
        "package test;\n"
        "import cajeta.lang.String;\n"
        "public final class Tag {\n"
        "    int32 n;\n"
        "    public Tag(int32 n) { this.n = n; }\n"
        "    public static #Tag make(int32 n) { return heap Tag(n); }\n"
        "    public #String render() { String s = \"tag \" + this.n; return #s; }\n"
        "}\n"
        "public final class U {\n"
        "    public static int32 run() {\n"
        "        String first #= Tag.make(0).render();\n"
        "        int64 live = Cajeta.liveCount();\n"
        "        int32 i = 0;\n"
        "        int32 len = 0;\n"
        "        while (i < 200) { String s #= Tag.make(i).render(); len = s.byteLength(); i = i + 1; }\n"
        "        return Cajeta.liveCount() == live ? len : -1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}
