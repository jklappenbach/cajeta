// title-stores 6.3.2 — String element slots ALWAYS own their wrappers, so a
// plain slot store copies (resolve) rather than aliasing. Pins the
// Headers.grow shape: a field-held String[] grown via a plain copy loop and
// a `#`-displacing swap must keep every element readable (the alias store
// left the new array pointing at wrappers the displacement walk freed).

#include "gtest/gtest.h"
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
}  // namespace

TEST(StringElemStoreTests, fieldGrowCopyLoopKeepsElements) {
    const char* src =
        "package test;\n"
        "public class Box {\n"
        "    String[] keys;\n"
        "    int32 cap;\n"
        "    int32 n;\n"
        "    public Box() { this.keys = heap String[4]; this.cap = 4; this.n = 0; }\n"
        "    public void add(String s) {\n"
        "        if (this.n >= this.cap) { this.grow(); }\n"
        "        this.keys[this.n] = s.toLowerCase();\n"
        "        this.n = this.n + 1;\n"
        "    }\n"
        "    void grow() {\n"
        "        int32 nc = this.cap * 2;\n"
        "        String[] dst = heap String[nc];\n"
        "        int32 i = 0;\n"
        "        while (i < this.n) {\n"
        "            dst[i] = this.keys[i];\n"
        "            i = i + 1;\n"
        "        }\n"
        "        this.keys = #dst;\n"
        "        this.cap = nc;\n"
        "    }\n"
        "    public int32 countEq(String name) {\n"
        "        int32 c = 0;\n"
        "        int32 i = 0;\n"
        "        while (i < this.n) {\n"
        "            if (this.keys[i].equals(name)) { c = c + 1; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return c;\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Box b = heap Box();\n"
        "        int32 i = 0;\n"
        "        while (i < 20) { b.add(\"Multi\"); i = i + 1; }\n"
        "        return b.countEq(\"multi\");\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 20);
}
