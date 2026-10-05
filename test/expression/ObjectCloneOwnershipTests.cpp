// field-store-ownership 6.2.5: a shallow Object.clone() borrows the class-typed, array and
// interface fields it shares, and owns a fresh stake on every String field, inherited ones too.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

std::string makeSource(const std::string& workBody) {
    return "package test;\n"
           "public class Cell {\n"
           "    public int64 n;\n"
           "    public Cell(int64 v) { this.n = v; }\n"
           "}\n"
           "public interface Shape {\n"
           "    public int64 area();\n"
           "}\n"
           "public class Sq implements Shape {\n"
           "    public int64 side;\n"
           "    public Sq(int64 s) { this.side = s; }\n"
           "    public int64 area() { return this.side * this.side; }\n"
           "}\n"
           "public class H {\n"
           "    public Cell c;\n"
           "    public int64[] xs;\n"
           "    public String s;\n"
           "    public H() { }\n"
           "}\n"
           "public class P {\n"
           "    public Cell c;\n"
           "    public String s;\n"
           "    public P() { }\n"
           "}\n"
           "public class Q extends P {\n"
           "    public int64 k;\n"
           "    public Q() { }\n"
           "}\n"
           "public class I {\n"
           "    public Shape sh;\n"
           "    public I() { }\n"
           "}\n"
           "public final class Ut {\n"
           "    public static #String heapString(int32 n) {\n"
           "        int8[] buf = Cajeta.allocBytes((int64) n);\n"
           "        int32 i = 0;\n"
           "        while (i < n) {\n"
           "            buf[i] = (int8) (97 + (i - (i / 26) * 26));\n"
           "            i = i + 1;\n"
           "        }\n"
           "        return heap String(#buf, n);\n"
           "    }\n"
           "    public static void churn() {\n"
           "        int64 i = 0;\n"
           "        while (i < 400L) {\n"
           "            Cell j = heap Cell(107800L);\n"
           "            String t #= Ut.heapString(40);\n"
           "            i = i + 1;\n"
           "        }\n"
           "    }\n"
           "    public static int32 work() {\n"
           "        " + workBody + "\n"
           "    }\n"
           "    public static int32 run() {\n"
           "        int64 base = Cajeta.liveCount();\n"
           "        int32 t = Ut.work();\n"
           "        int64 leaked = Cajeta.liveCount() - base;\n"
           "        return (int32) (leaked * 100) + t;\n"
           "    }\n"
           "}\n";
}

int32_t runJit(const std::string& workBody) {
    auto jit = CajetaJit::compile(makeSource(workBody), "test.Ut");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

}  // namespace

// The measured defect: dropping the clone freed the original's owned field.
TEST(ObjectCloneOwnershipTests, droppedCloneLeavesOwnedClassAndArrayFieldsAlive) {
    EXPECT_EQ(runJit(
        "H a = heap H();\n"
        "a.c #= heap Cell(8100L);\n"
        "a.xs #= heap int64[4];\n"
        "a.xs[3] = 77L;\n"
        "a.s #= Ut.heapString(40);\n"
        "{\n"
        "    H b #= a.clone();\n"
        "    if (b.c.n != 8100L || b.xs[3] != 77L || b.s.size() != 40) { return -1; }\n"
        "}\n"
        "Ut.churn();\n"
        "if (a.c.n != 8100L) { return -2; }\n"
        "if (a.xs[3] != 77L) { return -3; }\n"
        "if (a.s.size() != 40 || a.s.charAt(0) != (int8) 97) { return -4; }\n"
        "return 1;"), 1);
}

// The clone owns its String stakes, so it outlives the original's Strings.
TEST(ObjectCloneOwnershipTests, cloneStringFieldOutlivesTheOriginal) {
    EXPECT_EQ(runJit(
        "H b = heap H();\n"
        "{\n"
        "    H a = heap H();\n"
        "    a.s #= Ut.heapString(600);\n"
        "    b #= a.clone();\n"
        "}\n"
        "Ut.churn();\n"
        "if (b.s.size() != 600 || b.s.charAt(1) != (int8) 98) { return -1; }\n"
        "return 1;"), 1);
}

// Inherited fields: RTTI lists direct fields only, so the parent's must be fixed up too.
TEST(ObjectCloneOwnershipTests, inheritedOwnedFieldAndStringAreFixedUp) {
    EXPECT_EQ(runJit(
        "Q b = heap Q();\n"
        "Q a = heap Q();\n"
        "a.c #= heap Cell(8100L);\n"
        "{\n"
        "    Q t = heap Q();\n"
        "    t.s #= Ut.heapString(300);\n"
        "    b #= t.clone();\n"
        "}\n"
        "{\n"
        "    Q d #= a.clone();\n"
        "    if (d.c.n != 8100L) { return -1; }\n"
        "}\n"
        "Ut.churn();\n"
        "if (a.c.n != 8100L) { return -2; }\n"
        "if (b.s.size() != 300 || b.s.charAt(2) != (int8) 99) { return -3; }\n"
        "return 1;"), 1);
}

// An owned interface field is shared by reference, so the clone records it as borrowed.
TEST(ObjectCloneOwnershipTests, droppedCloneLeavesOwnedInterfaceFieldAlive) {
    EXPECT_EQ(runJit(
        "I a = heap I();\n"
        "a.sh #= heap Sq(9L);\n"
        "{\n"
        "    I b #= a.clone();\n"
        "    if (b.sh.area() != 81L) { return -1; }\n"
        "}\n"
        "Ut.churn();\n"
        "if (a.sh.area() != 81L) { return -2; }\n"
        "return 1;"), 1);
}

// Twin: a borrowed field stays borrowed in the clone, and nothing is freed twice.
TEST(ObjectCloneOwnershipTests, borrowedFieldStaysBorrowed) {
    EXPECT_EQ(runJit(
        "Cell x = heap Cell(5L);\n"
        "H a = heap H();\n"
        "a.c = x;\n"
        "{\n"
        "    H b #= a.clone();\n"
        "    if (b.c.n != 5L) { return -1; }\n"
        "}\n"
        "Ut.churn();\n"
        "if (x.n != 5L || a.c.n != 5L) { return -2; }\n"
        "return 1;"), 1);
}

// A clone reaches its inherited fields through its own base pointer, not the original's.
TEST(ObjectCloneOwnershipTests, cloneReadsItsOwnInheritedFields) {
    EXPECT_EQ(runJit(
        "Q a = heap Q();\n"
        "a.c #= heap Cell(1L);\n"
        "Q b #= a.clone();\n"
        "if (b.c.n != 1L) { return -1; }\n"
        "a.s #= Ut.heapString(30);\n"
        "if (b.s != null) { return -2; }\n"
        "return 1;"), 1);
}
