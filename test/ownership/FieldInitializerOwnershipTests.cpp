// A field initializer that tenders a title records it in the owner's ownership word,
// so the owner's drop frees what the initializer allocated, and a lent one stays lent.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.F");
    if (!jit) return -1;
    auto fn = jit->lookup<int32_t (*)()>("run");
    if (!fn) return -2;
    return fn();
}

// Builds 100 owners through `make` and returns the live-count growth after they drop.
std::string ownersLoop(const std::string& classes, const std::string& make) {
    return "package test;\n"
        "import cajeta.lang.Cajeta;\n"
        "import cajeta.lang.String;\n"
        "import cajeta.collection.ArrayList;\n"
        "public class Inner { public int32 v; }\n" + classes +
        "public final class F {\n"
        "    static int32 once(int32 i) {\n"
        "        " + make + "\n"
        "        return 0;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        F.once(0);\n"
        "        int64 l0 = Cajeta.liveCount();\n"
        "        int32 i = 0;\n"
        "        while (i < 100) { F.once(i); i = i + 1; }\n"
        "        return (int32) (Cajeta.liveCount() - l0);\n"
        "    }\n"
        "}\n";
}

} // namespace

TEST(FieldInitializerOwnershipTests, classInitializerDropsWithTheOwner) {
    EXPECT_EQ(runI32(ownersLoop("public class O { Inner x = heap Inner(); }\n",
                                "O o = heap O();")), 0);
}

TEST(FieldInitializerOwnershipTests, everyConstructorShapeRecordsTheTitle) {
    EXPECT_EQ(runI32(ownersLoop("public class O { Inner x = heap Inner(); public O() { } }\n",
                                "O o = heap O();")), 0);
    EXPECT_EQ(runI32(ownersLoop(
        "public class O { Inner x = heap Inner(); int32 k; public O(int32 k) { this.k = k; } }\n",
        "O o = heap O(i);")), 0);
}

TEST(FieldInitializerOwnershipTests, containerAndArrayInitializersDropWithTheOwner) {
    EXPECT_EQ(runI32(ownersLoop("public class O { ArrayList<Inner> xs = heap ArrayList<Inner>(); }\n",
                                "O o = heap O(); o.xs.add(heap Inner());")), 0);
    EXPECT_EQ(runI32(ownersLoop("public class O { Inner[] xs = heap Inner[2]; }\n",
                                "O o = heap O();")), 0);
    EXPECT_EQ(runI32(ownersLoop("public class O { String[] xs = heap String[2]; }\n",
                                "O o = heap O(); o.xs[0] = \"k\" + i;")), 0);
}

TEST(FieldInitializerOwnershipTests, stringInitializerDropsWithTheOwner) {
    EXPECT_EQ(runI32(ownersLoop("public class O { String s = \"value-\" + 7 + \"-abcdefghijklmnopqrstuvwxyz\"; }\n",
                                "O o = heap O();")), 0);
}

// A static field lent to 100 owners is still intact once they have all dropped.
TEST(FieldInitializerOwnershipTests, lentInitializerIsNotFreedByTheOwner) {
    std::string src =
        "package test;\n"
        "import cajeta.lang.Cajeta;\n"
        "public class Inner { public int32 v; }\n"
        "public class Keep { public static Inner shared = heap Inner(); }\n"
        "public class O { Inner x = Keep.shared; }\n"
        "public final class F {\n"
        "    public static int32 run() {\n"
        "        Keep.shared.v = 4242;\n"
        "        int64 l0 = Cajeta.liveCount();\n"
        "        int32 i = 0;\n"
        "        while (i < 100) { O o = heap O(); i = i + 1; }\n"
        "        int32 j = 0;\n"
        "        while (j < 100) { Inner churn = heap Inner(); churn.v = 1; j = j + 1; }\n"
        "        if (Cajeta.liveCount() != l0) { return -3; }\n"
        "        return Keep.shared.v;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 4242);
}
