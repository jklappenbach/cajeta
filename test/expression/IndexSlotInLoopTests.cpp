// An index or window read inside a loop must not grow the stack per trip:
// each such read's value slot belongs in the entry block, not at the read.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int64_t runI64(const std::string& body) {
    std::string src = std::string(
        "package test;\n"
        "public final class L {\n"
        "    static int64 sum(Slice<int8> s) {\n"
        "        int64 t = 0;\n"
        "        int64 i = 0;\n"
        "        while (i < s.count()) { t = t + (int64) s[i]; i = i + 1; }\n"
        "        return t;\n"
        "    }\n"
        "    static async int64 sumOnFiber(int8[] a, int32 n) {\n"
        "        return L.sum(a[0:n]);\n"
        "    }\n"
        "    static int8[] ones(int32 n) {\n"
        "        int8[] a = heap int8[n];\n"
        "        int32 i = 0;\n"
        "        while (i < n) { a[i] = (int8) 1; i = i + 1; }\n"
        "        return a;\n"
        "    }\n"
        "    public static int64 run() {\n")
        + body
        + "    }\n"
          "}\n";
    auto jit = CajetaJit::compile(src, "test.L");
    return jit->lookup<int64_t (*)()>("run")();
}

}  // namespace

TEST(IndexSlotInLoopTests, sliceReadsInALongLoopKeepTheStackFlat) {
    EXPECT_EQ(runI64("        int8[] a = heap int8[4000000];\n"
                     "        int32 i = 0;\n"
                     "        while (i < 4000000) { a[i] = (int8) 1; i = i + 1; }\n"
                     "        return L.sum(a[0:4000000]);\n"),
              4000000);
}

TEST(IndexSlotInLoopTests, sliceReadsInALoopOnAFiberStayWithinItsStack) {
    EXPECT_EQ(runI64("        int8[] a = heap int8[300000];\n"
                     "        int32 i = 0;\n"
                     "        while (i < 300000) { a[i] = (int8) 1; i = i + 1; }\n"
                     "        Task<int64> t = spawn sumOnFiber(a, 300000);\n"
                     "        return await t;\n"),
              300000);
}

TEST(IndexSlotInLoopTests, aWindowMadeInALongLoopKeepsTheStackFlat) {
    EXPECT_EQ(runI64("        int8[] a = heap int8[16];\n"
                     "        int32 k = 0;\n"
                     "        while (k < 16) { a[k] = (int8) k; k = k + 1; }\n"
                     "        int64 t = 0;\n"
                     "        int32 i = 0;\n"
                     "        while (i < 2000000) {\n"
                     "            Slice<int8> w = a[i % 16:i % 16 + 1];\n"
                     "            t = t + (int64) w[0];\n"
                     "            i = i + 1;\n"
                     "        }\n"
                     "        return t;\n"),
              (int64_t) 2000000 / 16 * (0 + 15) * 16 / 2);
}
