//
// ownership-title-classifier Unit 4 — the local declaration on the classifier.
//
// 4.1.1 (spec 5.7): an interface-typed local bound from a fresh construction
// or a `#R` call OWNS it. The declaration's interface-slot rule counted only a
// `#x` move as owned, so `Shape s = heap Square(3);` recorded a BORROWED kind
// and the square leaked with the frame.
//
// Verdicts read back through the runtime: Cajeta.liveCount() balanced over a
// loop of calls.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runVerdict(const char* src, const char* cls) {
    auto jit = CajetaJit::compile(src, cls);
    if (!jit) return -1;
    auto fn = jit->lookup<int32_t (*)()>("run");
    if (!fn) return -2;
    return fn();
}

const char* PRE =
    "package test;\n"
    "import cajeta.lang.Cajeta;\n"
    "import cajeta.lang.String;\n"
    "public interface Shape {\n"
    "    int32 area();\n"
    "}\n"
    "public class Square implements Shape {\n"
    "    public int32 side;\n"
    "    public Square(int32 s) { this.side = s; }\n"
    "    public int32 area() { return this.side * this.side; }\n"
    "}\n"
    "public final class A {\n"
    "    static #Shape mk(int32 s) { return heap Square(s); }\n";

} // namespace

// `Shape s = heap Square(3);` — the fresh square is the local's to drop.
TEST(DeclarationOwnershipTests, interfaceLocalFromFreshConstructionOwnsIt) {
    std::string src = std::string(PRE) +
        "    static int32 probe() {\n"
        "        Shape s = heap Square(3);\n"
        "        return s.area();\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int64 l0 = Cajeta.liveCount();\n"
        "        int32 i = 0;\n"
        "        while (i < 64) {\n"
        "            if (A.probe() != 9) { return 1; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        if (Cajeta.liveCount() != l0) { return 2; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runVerdict(src.c_str(), "test.A"), 0)
        << "1 = wrong value; 2 = the squares leaked (interface slot recorded a borrow)";
}

// `Shape s #= A.mk(4);` — a `#Shape` result is the local's to drop.
TEST(DeclarationOwnershipTests, interfaceLocalFromOwnedCallOwnsIt) {
    std::string src = std::string(PRE) +
        "    static int32 probe() {\n"
        "        Shape s #= A.mk(4);\n"
        "        return s.area();\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int64 l0 = Cajeta.liveCount();\n"
        "        int32 i = 0;\n"
        "        while (i < 64) {\n"
        "            if (A.probe() != 16) { return 3; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        if (Cajeta.liveCount() != l0) { return 4; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runVerdict(src.c_str(), "test.A"), 0)
        << "3 = wrong value; 4 = the squares leaked";
}

// A borrowed interface local (a field read) must not drop what it borrows.
TEST(DeclarationOwnershipTests, interfaceLocalFromFieldReadBorrows) {
    std::string src = std::string(PRE) +
        "    static class Box {\n"
        "        public Shape s;\n"
        "        public Box() { this.s #= heap Square(5); return; }\n"
        "    }\n"
        "    static int32 probe(Box b) {\n"
        "        Shape v = b.s;\n"
        "        return v.area();\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Box b = heap Box();\n"
        "        int64 l0 = Cajeta.liveCount();\n"
        "        int32 i = 0;\n"
        "        while (i < 64) {\n"
        "            if (A.probe(b) != 25) { return 5; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        if (Cajeta.liveCount() != l0) { return 6; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runVerdict(src.c_str(), "test.A"), 0)
        << "5 = wrong value (the borrowed square was freed); 6 = live count moved";
}

// 4.1.2 — a String bound from a PLAIN call rides the callee's flag: a
// tail call through a plain wrapper hands out a title (ownership §2.1), and
// the binding now arms the string drop from that flag. Before this the
// binding had no entry at all and every such String leaked.
TEST(DeclarationOwnershipTests, stringFromPlainCallRideIsFreed) {
    std::string src = std::string(PRE) +
        "    static #String mkS(int32 i) { return \"v\" + i; }\n"
        "    static String via(int32 i) { return A.mkS(i); }\n"
        "    static class Box { public String name; public Box() { this.name #= \"boxed\" + 1; return; } }\n"
        "    static String lend(Box b) { return b.name; }\n"
        "    static int32 probe(int32 i, Box b) {\n"
        "        String s = A.via(i);\n"
        "        String t = A.lend(b);\n"
        "        return s.byteLength() + t.byteLength();\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Box b = heap Box();\n"
        "        int64 l0 = Cajeta.liveCount();\n"
        "        int32 i = 0;\n"
        "        while (i < 64) {\n"
        "            if (A.probe(i, b) < 8) { return 7; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        if (Cajeta.liveCount() != l0) { return 8; }\n"
        "        if (b.name.byteLength() != 6) { return 9; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runVerdict(src.c_str(), "test.A"), 0)
        << "7 = wrong value; 8 = the ride-through Strings leaked (or the lent one was freed); "
           "9 = the lent String was freed";
}

// 4.1.2 — closure locals: a fresh lambda OWNS its record, an alias (`fn g =
// f`) borrows it, a `#=` of the owner moves it. One drop per record.
TEST(DeclarationOwnershipTests, closureAliasBorrowsAndMoveTransfers) {
    std::string src = std::string(PRE) +
        "    static int32 probe(int32 cap) {\n"
        "        (int32) -> int32 f = (int32 x) -> x + cap;\n"
        "        (int32) -> int32 g = f;\n"
        "        int32 a = g(2);\n"
        "        (int32) -> int32 h #= f;\n"
        "        return a + h(3);\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int32 i = 0;\n"
        "        while (i < 64) {\n"
        "            if (A.probe(i) != 2 * i + 5) { return 10; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runVerdict(src.c_str(), "test.A"), 0)
        << "10 = wrong value (a double free would abort the process)";
}

// 4.1.2 — arrays: a field read borrows (the owner's array survives the
// borrowing frame), a plain-call ride is freed, a plain-call lend is not.
TEST(DeclarationOwnershipTests, arrayLocalBorrowAndRideShapes) {
    std::string src = std::string(PRE) +
        "    static class Box { public int32[] data; public Box() { this.data #= heap int32[4]; this.data[0] = 7; return; } }\n"
        "    static #int32[] mkA(int32 i) { int32[] a = heap int32[3]; a[0] = i; return #a; }\n"
        "    static int32[] via(int32 i) { return A.mkA(i); }\n"
        "    static int32[] lend(Box b) { return b.data; }\n"
        "    static int32 probe(int32 i, Box b) {\n"
        "        int32[] v = b.data;\n"
        "        int32[] r = A.via(i);\n"
        "        int32[] l = A.lend(b);\n"
        "        return v[0] + r[0] + l[0];\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Box b = heap Box();\n"
        "        int32 i = 0;\n"
        "        while (i < 64) {\n"
        "            if (A.probe(i, b) != 14 + i) { return 11; }\n"
        "            int32[] churn = heap int32[4];\n"
        "            churn[0] = -1;\n"
        "            i = i + 1;\n"
        "        }\n"
        "        if (b.data[0] != 7) { return 12; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runVerdict(src.c_str(), "test.A"), 0)
        << "11 = wrong value; 12 = the borrowed array was freed under its owner";
}

// 4.1.4(e) — a local bound from a PLAIN call now carries a flagged entry;
// storing it into a String array must take what the local HOLDS (the entry's
// flag), not "it has an entry". The borrowed column name survives the frame
// that stored it; the fresh one is owned exactly once.
TEST(DeclarationOwnershipTests, stringElementStoreTakesOnlyAHeldTitle) {
    std::string src = std::string(PRE) +
        "    static class Node { public String name; public Node() { this.name #= \"ts\" + 1; return; } }\n"
        "    static String nameOf(Node n) { return n.name; }\n"
        "    static #String fresh(int32 i) { return \"f\" + i; }\n"
        "    static String viaFresh(int32 i) { return A.fresh(i); }\n"
        "    static int32 probe(Node n, int32 i) {\n"
        "        String b = A.nameOf(n);\n"
        "        String f = A.viaFresh(i);\n"
        "        String[] names = heap String[2];\n"
        "        names[0] = b;\n"
        "        names[1] = f;\n"
        "        return names[0].byteLength() + names[1].byteLength();\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Node n = heap Node();\n"
        "        int64 l0 = Cajeta.liveCount();\n"
        "        int32 i = 0;\n"
        "        while (i < 64) {\n"
        "            if (A.probe(n, i) < 5) { return 13; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        if (n.name.byteLength() != 3) { return 14; }\n"
        "        if (Cajeta.liveCount() != l0) { return 15; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runVerdict(src.c_str(), "test.A"), 0)
        << "13 = wrong value; 14 = the borrowed name was freed by the array (taken as owned); "
           "15 = the fresh Strings leaked or the borrowed one was double-titled";
}
