// An ArrayList of an interface type owns the elements handed to it and frees
// them when it drops, with each element's ownership carried in its own body.

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

// `body()` runs 100 times after a warm-up; returns its value when the live
// count held, -1 when it moved.
std::string program(const std::string& body) {
    return
        "package test;\n"
        "import cajeta.collection.ArrayList;\n"
        "import cajeta.lang.String;\n"
        "interface I { int32 f(); }\n"
        "final class A implements I {\n"
        "    int32 v;\n"
        "    A(int32 v) { this.v = v; }\n"
        "    public int32 f() { return this.v; }\n"
        "}\n"
        "final class Holder {\n"
        "    ArrayList<I> xs;\n"
        "    Holder() { this.xs = heap ArrayList<I>(); }\n"
        "    void add(#I x) { this.xs.add(#x); }\n"
        "}\n"
        "public final class U {\n"
        "    static int32 body() {\n" + body +
        "    }\n"
        "    public static int32 run() {\n"
        "        int32 last = U.body();\n"
        "        int64 live = Cajeta.liveCount();\n"
        "        int32 i = 0;\n"
        "        while (i < 100) { last = U.body(); i = i + 1; }\n"
        "        return Cajeta.liveCount() == live ? last : -1;\n"
        "    }\n"
        "}\n";
}

} // namespace

TEST(InterfaceListOwnershipTests, freshElementsDropWithTheList) {
    EXPECT_EQ(runI32(program(
        "        ArrayList<I> xs = heap ArrayList<I>();\n"
        "        xs.add(heap A(1)); xs.add(heap A(2));\n"
        "        return xs.get(0).f() + xs.get(1).f();\n")), 3);
}

TEST(InterfaceListOwnershipTests, movedLocalsDropWithTheList) {
    EXPECT_EQ(runI32(program(
        "        ArrayList<I> xs = heap ArrayList<I>();\n"
        "        A a = heap A(4);\n"
        "        xs.add(#a);\n"
        "        return xs.get(0).f();\n")), 4);
}

TEST(InterfaceListOwnershipTests, elementsForwardedThroughAnOwnedFormal) {
    EXPECT_EQ(runI32(program(
        "        Holder h = heap Holder();\n"
        "        h.add(heap A(5)); h.add(heap A(6));\n"
        "        return h.xs.get(1).f();\n")), 6);
}

TEST(InterfaceListOwnershipTests, borrowedElementsAreNotFreedByTheList) {
    EXPECT_EQ(runI32(program(
        "        A a = heap A(7);\n"
        "        ArrayList<I> xs = heap ArrayList<I>();\n"
        "        xs.add(a);\n"
        "        int32 r = xs.get(0).f();\n"
        "        xs = heap ArrayList<I>();\n"
        "        return r + a.f();\n")), 14);
}

TEST(InterfaceListOwnershipTests, removedElementBelongsToTheCaller) {
    EXPECT_EQ(runI32(program(
        "        ArrayList<I> xs = heap ArrayList<I>();\n"
        "        xs.add(heap A(8)); xs.add(heap A(9));\n"
        "        I out #= xs.removeAt(0);\n"
        "        return out.f() + xs.count();\n")), 9);
}

TEST(InterfaceListOwnershipTests, clearDropsTheElements) {
    EXPECT_EQ(runI32(program(
        "        ArrayList<I> xs = heap ArrayList<I>();\n"
        "        xs.add(heap A(1)); xs.add(heap A(2));\n"
        "        xs.clear();\n"
        "        xs.add(heap A(3));\n"
        "        return xs.get(0).f();\n")), 3);
}

TEST(InterfaceListOwnershipTests, growthMovesElementsWithoutDoubleFreeing) {
    EXPECT_EQ(runI32(program(
        "        ArrayList<I> xs = heap ArrayList<I>();\n"
        "        int32 k = 0;\n"
        "        while (k < 40) { xs.add(heap A(k)); k = k + 1; }\n"
        "        String churn = \"churn churn churn churn churn churn churn churn\";\n"
        "        int32 t = 0;\n"
        "        k = 0;\n"
        "        while (k < 40) { t = t + xs.get(k).f(); k = k + 1; }\n"
        "        return t;\n")), 780);
}

TEST(InterfaceListOwnershipTests, setReleasesTheDisplacedElement) {
    EXPECT_EQ(runI32(program(
        "        ArrayList<I> xs = heap ArrayList<I>();\n"
        "        xs.add(heap A(1));\n"
        "        xs.set(0, heap A(9));\n"
        "        return xs.get(0).f();\n")), 9);
}

// DISABLED: an interface-typed formal ignores its transfer bit, so `this.val #= v` in a
// generic class records a borrow and the map never frees the value.
TEST(InterfaceListOwnershipTests, DISABLED_mapValuesDropWithTheMap) {
    EXPECT_EQ(runI32(program(
        "        cajeta.collection.HashMap<String, I> m = heap cajeta.collection.HashMap<String, I>();\n"
        "        String k1 = \"one\";\n"
        "        String k2 = \"two\";\n"
        "        m.put(k1, heap A(1));\n"
        "        m.put(k2, heap A(2));\n"
        "        return m.get(\"two\").f();\n")), 2);
}
