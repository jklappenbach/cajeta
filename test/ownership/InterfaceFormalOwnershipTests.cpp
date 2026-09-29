// An interface-typed formal holds the title its caller's transfer bit tendered: the
// callee drops it at return unless a store took it, and a generic `T` field or a map
// slot of interface type drops by the body's kind tag.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.Y");
    if (!jit) return -1;
    auto fn = jit->lookup<int32_t (*)()>("run");
    if (!fn) return -2;
    return fn();
}

const char* PRE =
    "package test;\n"
    "import cajeta.lang.Cajeta;\n"
    "import cajeta.lang.String;\n"
    "import cajeta.collection.HashMap;\n"
    "interface Sh { int32 f(); }\n"
    "final class Sq implements Sh {\n"
    "    int32 v;\n"
    "    Sq(int32 v) { this.v = v; }\n"
    "    public int32 f() { return this.v; }\n"
    "}\n"
    "final class GBox<T> {\n"
    "    T v;\n"
    "    void set(T x) { this.v #= x; }\n"
    "}\n"
    "final class IBox {\n"
    "    Sh v;\n"
    "    void set(Sh x) { this.v #= x; }\n"
    "}\n"
    "public final class Y {\n"
    "    static int32 look(Sh x) { return x.f(); }\n"
    "    static int32 eat(#Sh x) { return x.f(); }\n";

// Runs `body` 100 times after a warm-up; returns the live-count growth.
std::string growth(const std::string& body) {
    return std::string(PRE) +
        "    static void once(int32 i) {\n        " + body + "\n    }\n"
        "    public static int32 run() {\n"
        "        Y.once(0);\n"
        "        int64 l0 = Cajeta.liveCount();\n"
        "        int32 i = 0;\n"
        "        while (i < 100) { Y.once(i); i = i + 1; }\n"
        "        return (int32) (Cajeta.liveCount() - l0);\n"
        "    }\n"
        "}\n";
}

} // namespace

TEST(InterfaceFormalOwnershipTests, tenderedTitleDropsAtReturn) {
    EXPECT_EQ(runI32(growth("Y.look(heap Sq(i));")), 0);
    EXPECT_EQ(runI32(growth("Y.eat(heap Sq(i));")), 0);
    EXPECT_EQ(runI32(growth("Sh s = heap Sq(i); Y.eat(#s);")), 0);
    EXPECT_EQ(runI32(growth("Sh s = heap Sq(i); Y.look(s);")), 0);
}

TEST(InterfaceFormalOwnershipTests, storedTitleDropsWithItsHolder) {
    EXPECT_EQ(runI32(growth("IBox b = heap IBox(); b.set(heap Sq(i));")), 0);
    EXPECT_EQ(runI32(growth("GBox<Sh> b = heap GBox<Sh>(); b.set(heap Sq(i));")), 0);
    EXPECT_EQ(runI32(growth(
        "HashMap<String, Sh> m = heap HashMap<String, Sh>(); m.put(\"a\", heap Sq(i)); m.put(\"b\", heap Sq(i));")), 0);
}

// A lent value passes through a formal and a generic holder and is still intact.
TEST(InterfaceFormalOwnershipTests, lentValueSurvivesItsBorrowers) {
    std::string src = std::string(PRE) +
        "    public static int32 run() {\n"
        "        Sq keep = heap Sq(77);\n"
        "        int32 i = 0;\n"
        "        while (i < 100) {\n"
        "            Y.look(keep);\n"
        "            GBox<Sh> b = heap GBox<Sh>();\n"
        "            b.set(keep);\n"
        "            IBox c = heap IBox();\n"
        "            c.set(keep);\n"
        "            i = i + 1;\n"
        "        }\n"
        "        int32 j = 0;\n"
        "        while (j < 100) { Sq churn = heap Sq(1); j = j + 1; }\n"
        "        return keep.f();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 77);
}

// A map rehash moves each value's title to the new table: every value survives it.
TEST(InterfaceFormalOwnershipTests, mapValuesSurviveRehash) {
    std::string src = std::string(PRE) +
        "    public static int32 run() {\n"
        "        int64 l0 = Cajeta.liveCount();\n"
        "        int32 t = 0;\n"
        "        {\n"
        "            HashMap<String, Sh> m = heap HashMap<String, Sh>();\n"
        "            int32 i = 0;\n"
        "            while (i < 200) { String k #= \"k\" + i; m.put(#k, heap Sq(i)); i = i + 1; }\n"
        "            i = 0;\n"
        "            while (i < 200) { String k #= \"k\" + i; t = t + m.get(k).f(); i = i + 1; }\n"
        "        }\n"
        "        if (Cajeta.liveCount() != l0) { return -3; }\n"
        "        return t;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 19900);
}

// The body built for an interface argument reuses one frame slot: two million calls
// in a loop used to overflow the stack.
TEST(InterfaceFormalOwnershipTests, interfaceArgumentBodiesDoNotGrowTheStack) {
    std::string src = std::string(PRE) +
        "    public static int32 run() {\n"
        "        Sq s = heap Sq(1);\n"
        "        int64 t = 0;\n"
        "        int32 i = 0;\n"
        "        while (i < 2000000) { t = t + (int64) Y.look(s); i = i + 1; }\n"
        "        return t == (int64) 2000000 ? 1 : 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// Keeping a plain interface formal in a field is rejected, as it is for a class formal.
TEST(InterfaceFormalOwnershipTests, plainFormalKeptInAFieldIsRejected) {
    std::string src = std::string(PRE) +
        "    public static int32 run() { return 0; }\n"
        "}\n"
        "final class PBox {\n"
        "    Sh v;\n"
        "    void set(Sh x) { this.v = x; }\n"
        "}\n";
    EXPECT_ANY_THROW(CajetaJit::compile(src, "test.Y"));
}
