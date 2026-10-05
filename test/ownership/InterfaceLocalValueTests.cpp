// An interface local owns one body: initialization and assignment copy into it, the
// kind word carries the title, and a loop reuses the same frame slot every iteration.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.V");
    if (!jit) return -1;
    auto fn = jit->lookup<int32_t (*)()>("run");
    if (!fn) return -2;
    return fn();
}

const char* PRE =
    "package test;\n"
    "import cajeta.lang.Cajeta;\n"
    "import cajeta.collection.ArrayList;\n"
    "interface Sh { int32 f(); }\n"
    "final class Sq implements Sh {\n"
    "    int32 v;\n"
    "    Sq(int32 v) { this.v = v; }\n"
    "    public int32 f() { return this.v; }\n"
    "}\n"
    "final class Keep { public static Sh shared; }\n"
    "final class Box {\n"
    "    Sh v;\n"
    "    Box(#Sh p) { if (p.f() < 0) { p = Keep.shared; } this.v #= p; }\n"
    "}\n"
    "public final class V {\n"
    "    static Sh pick(Sh a) { return a; }\n"
    "    static #Sh mk(int32 i) { return heap Sq(i); }\n"
    "    static int32 take(#Sh s) { return s.f(); }\n"
    "    static int32 swap(#Sh s) { s = heap Sq(5); return s.f(); }\n"
    "    static int32 walk(Sh p) {\n"
    "        int32 acc = 0;\n"
    "        int32 i = 0;\n"
    "        while (i < 1000) { p = heap Sq(i % 3); acc = acc + p.f(); i = i + 1; }\n"
    "        return acc;\n"
    "    }\n";

std::string body(const std::string& run) {
    return std::string(PRE) + "    public static int32 run() {\n        Keep.shared = heap Sq(77);\n" + run + "    }\n}\n";
}

} // namespace

TEST(InterfaceLocalValueTests, loopLocalsDoNotGrowTheStack) {
    EXPECT_EQ(runI32(body(
        "        int64 l0 = Cajeta.liveCount();\n"
        "        Sh keep = heap Sq(3);\n"
        "        int64 acc = 0;\n"
        "        int32 i = 0;\n"
        "        while (i < 3000000) {\n"
        "            Sh r #= V.mk(i % 7);\n"
        "            Sh q = heap Sq(1);\n"
        "            acc = acc + r.f() + q.f() + V.pick(keep).f();\n"
        "            i = i + 1;\n"
        "        }\n"
        "        if (Cajeta.liveCount() != l0 + 1) { return -7; }\n"
        "        return (int32) (acc % 1000);\n")), 994);
}

TEST(InterfaceLocalValueTests, assignmentKeepsTheChosenValue) {
    EXPECT_EQ(runI32(body(
        "        ArrayList<Sh> rows = heap ArrayList<Sh>();\n"
        "        rows.add(heap Sq(4)); rows.add(heap Sq(9)); rows.add(heap Sq(2));\n"
        "        Sh best = null;\n"
        "        int32 i = 0;\n"
        "        while (i < rows.count()) {\n"
        "            Sh r = rows.get(i);\n"
        "            if (best == null || r.f() > best.f()) { best = r; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return best.f();\n")), 9);
}

TEST(InterfaceLocalValueTests, classAssignedIntoADeclaredLocalDispatches) {
    EXPECT_EQ(runI32(body(
        "        Sh c;\n"
        "        c = heap Sq(5);\n"
        "        Sh d = V.pick(c);\n"
        "        d = heap Sq(6);\n"
        "        return c.f() * 10 + d.f();\n")), 56);
}

TEST(InterfaceLocalValueTests, reassignmentDropsTheOldOwnedValue) {
    EXPECT_EQ(runI32(body(
        "        int64 l0 = Cajeta.liveCount();\n"
        "        int32 i = 0;\n"
        "        while (i < 100) {\n"
        "            Sh c = heap Sq(1);\n"
        "            c = heap Sq(2);\n"
        "            c #= V.mk(3);\n"
        "            c = c;\n"
        "            Sh o = heap Sq(7);\n"
        "            c #= o;\n"
        "            Sh b = Keep.shared;\n"
        "            b #= V.mk(4);\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return (int32) (Cajeta.liveCount() - l0);\n")), 0);
}

TEST(InterfaceLocalValueTests, movedOutValueIsNotDroppedByReassignment) {
    EXPECT_EQ(runI32(body(
        "        ArrayList<Sh> kept = heap ArrayList<Sh>();\n"
        "        int64 l0 = Cajeta.liveCount();\n"
        "        int32 i = 0;\n"
        "        while (i < 50) {\n"
        "            Sh a = heap Sq(i);\n"
        "            kept.add(#a);\n"
        "            a = heap Sq(1000);\n"
        "            Sh t = heap Sq(i);\n"
        "            V.take(#t);\n"
        "            t = heap Sq(2000);\n"
        "            i = i + 1;\n"
        "        }\n"
        "        int32 j = 0;\n"
        "        while (j < 200) { Sh churn = heap Sq(-1); j = j + 1; }\n"
        "        int32 sum = 0;\n"
        "        i = 0;\n"
        "        while (i < kept.count()) { sum = sum + kept.get(i).f(); i = i + 1; }\n"
        "        if (Cajeta.liveCount() != l0 + 50) { return -3; }\n"
        "        return sum;\n")), 1225);
}

TEST(InterfaceLocalValueTests, nullAndStaticSourcesCopy) {
    EXPECT_EQ(runI32(body(
        "        Sh a = null;\n"
        "        Sh b = a;\n"
        "        Sh c = heap Sq(1);\n"
        "        c = null;\n"
        "        Sh s = Keep.shared;\n"
        "        Sh t = heap Sq(2);\n"
        "        t = Keep.shared;\n"
        "        int32 r = 0;\n"
        "        if (a == null) { r = r + 1; }\n"
        "        if (b == null) { r = r + 10; }\n"
        "        if (c == null) { r = r + 100; }\n"
        "        return r * 1000 + s.f() + t.f();\n")), 111154);
}

TEST(InterfaceLocalValueTests, assignedParametersOwnTheirBody) {
    EXPECT_EQ(runI32(body(
        "        Sh a = heap Sq(1);\n"
        "        int64 l0 = Cajeta.liveCount();\n"
        "        int32 r = V.walk(a);\n"
        "        int32 i = 0;\n"
        "        while (i < 100) {\n"
        "            r = r + V.swap(heap Sq(i));\n"
        "            Box kept = heap Box(heap Sq(i));\n"
        "            Box lent = heap Box(heap Sq(-1));\n"
        "            r = r + kept.v.f() - lent.v.f();\n"
        "            i = i + 1;\n"
        "        }\n"
        "        if (Cajeta.liveCount() != l0) { return -3; }\n"
        "        return r + a.f() * 100000 + Keep.shared.f() * 1000000;\n")), 77098749);
}

TEST(InterfaceLocalValueTests, staticInterfaceFieldTakesAnInitializer) {
    std::string src =
        "package test;\n"
        "interface Sh { int32 f(); }\n"
        "final class Sq implements Sh {\n"
        "    int32 v;\n"
        "    Sq(int32 v) { this.v = v; }\n"
        "    public int32 f() { return this.v; }\n"
        "}\n"
        "final class Reg {\n"
        "    public static Sh fromClass = heap Sq(3);\n"
        "    public static Sh fromCall = Reg.mk(4);\n"
        "    public static Sh copied = Reg.fromClass;\n"
        "    public static Sh none = null;\n"
        "    static #Sh mk(int32 i) { return heap Sq(i); }\n"
        "}\n"
        "public final class V {\n"
        "    public static int32 run() {\n"
        "        int32 n = 0;\n"
        "        if (Reg.none == null) { n = 1; }\n"
        "        return Reg.fromClass.f() * 100 + Reg.fromCall.f() * 10 + Reg.copied.f() + n * 1000;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1343);
}
