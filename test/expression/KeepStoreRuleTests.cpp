// field-store-ownership Units 4 and 5 (spec 3, 4, 6): a kept formal or owned local is
// stored with `#=`, a formal kept with `=` is `^T`, and the checks run before codegen.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <string>

#include "cajeta/error/Exception.h"

using cajeta_test::CajetaJit;

namespace {

const char* kPre =
    "package test;\n"
    "public class Cell {\n"
    "    public int64 n;\n"
    "    public Cell(int64 v) { this.n = v; }\n"
    "}\n"
    "public class Node {\n"
    "    public Cell c;\n"
    "    public Node prev;\n"
    "    public Node() { }\n"
    "}\n";

const char* kRun =
    "public final class D {\n"
    "    public static #Cell fresh() { return heap Cell(1L); }\n"
    "    public static Cell plainMake() { return D.fresh(); }\n"
    "    public static int64 run() { return 0L; }\n"
    "}\n";

std::string expectError(const std::string& body, const std::string& code) {
    try {
        CajetaJit::compile(std::string(kPre) + body + kRun, "test.D");
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), code) << e.getMessage();
        return e.getMessage();
    } catch (const std::exception& e) {
        ADD_FAILURE() << "expected " << code << ", got " << e.what();
        return e.what();
    }
    ADD_FAILURE() << "expected " << code << ", compiled cleanly";
    return "";
}

void expectCompiles(const std::string& body) {
    EXPECT_NO_THROW(CajetaJit::compile(std::string(kPre) + body + kRun, "test.D"));
}

}  // namespace

// 3.2.2
TEST(KeepStoreRuleTests, plainFormalPlainStoreRejected) {
    std::string msg = expectError(
        "public final class K {\n"
        "    Cell c;\n"
        "    public K() { }\n"
        "    public void set(Cell p) { this.c = p; }\n"
        "}\n",
        "CAJETA_ERROR_KEEP_NEEDS_SHARP_STORE");
    EXPECT_NE(msg.find("`p`"), std::string::npos) << msg;
    EXPECT_NE(msg.find("#="), std::string::npos) << msg;
    EXPECT_NE(msg.find("`^Cell`"), std::string::npos) << msg;
}

// 3.1: a slot store follows the same table.
TEST(KeepStoreRuleTests, plainFormalPlainSlotStoreRejected) {
    expectError(
        "public final class K {\n"
        "    Cell[] cs;\n"
        "    public K() { this.cs = heap Cell[2]; }\n"
        "    public void put(int32 i, Cell p) { this.cs[i] = p; }\n"
        "}\n",
        "CAJETA_ERROR_KEEP_NEEDS_SHARP_STORE");
}

// 3.1: a nested path follows the same table.
TEST(KeepStoreRuleTests, plainFormalNestedStoreRejected) {
    expectError(
        "public final class K {\n"
        "    Node head;\n"
        "    public K() { this.head = heap Node(); }\n"
        "    public void link(Node p) { this.head.prev = p; }\n"
        "}\n",
        "CAJETA_ERROR_KEEP_NEEDS_SHARP_STORE");
}

// 3.1: storing into a formal's field keeps the value as surely as into this.
TEST(KeepStoreRuleTests, plainFormalIntoAnotherFormalsFieldRejected) {
    expectError(
        "public final class K {\n"
        "    public K() { }\n"
        "    public static void attach(Node n, Cell p) { n.c = p; }\n"
        "}\n",
        "CAJETA_ERROR_KEEP_NEEDS_SHARP_STORE");
}

// 3.2.3
TEST(KeepStoreRuleTests, plainFormalSharpStoreCompiles) {
    expectCompiles(
        "public final class K {\n"
        "    Cell c;\n"
        "    public K() { }\n"
        "    public void set(Cell p) { this.c #= p; }\n"
        "}\n");
}

// 3.2.1
TEST(KeepStoreRuleTests, borrowFormalPlainStoreCompiles) {
    expectCompiles(
        "public final class K {\n"
        "    Cell c;\n"
        "    public K() { }\n"
        "    public void set(^Cell p) { this.c = p; }\n"
        "}\n");
}

// 3.2.4 and 6.1: the fix for a `#T` formal is `#=`, never `#T` alone.
TEST(KeepStoreRuleTests, sharpFormalPlainStoreRejected) {
    std::string msg = expectError(
        "public final class K {\n"
        "    Cell c;\n"
        "    public K() { }\n"
        "    public void take(#Cell p) { this.c = p; }\n"
        "}\n",
        "CAJETA_ERROR_KEEP_NEEDS_SHARP_STORE");
    EXPECT_NE(msg.find("#="), std::string::npos) << msg;
}

// 3.2.5
TEST(KeepStoreRuleTests, stringFormalPlainStoreRejected) {
    expectError(
        "public final class K {\n"
        "    String v;\n"
        "    public K() { }\n"
        "    public void put(String p) { this.v = p; }\n"
        "}\n",
        "CAJETA_ERROR_KEEP_NEEDS_SHARP_STORE");
}

// 3.2.6
TEST(KeepStoreRuleTests, primitiveFormalPlainStoreCompiles) {
    expectCompiles(
        "public final class K {\n"
        "    int64 n;\n"
        "    public K() { }\n"
        "    public void set(int64 p) { this.n = p; }\n"
        "}\n");
}

// 3.1: an alias local of a formal is the formal.
TEST(KeepStoreRuleTests, formalThroughAliasLocalRejected) {
    expectError(
        "public final class K {\n"
        "    Cell c;\n"
        "    public K() { }\n"
        "    public void set(Cell p) { Cell l = p; this.c = l; }\n"
        "}\n",
        "CAJETA_ERROR_KEEP_NEEDS_SHARP_STORE");
}

// 3.1: a static is kept beyond the call.
TEST(KeepStoreRuleTests, plainFormalIntoStaticRejected) {
    expectError(
        "public final class K {\n"
        "    static Cell s;\n"
        "    public K() { }\n"
        "    public static void set(Cell p) { s = p; }\n"
        "}\n",
        "CAJETA_ERROR_KEEP_NEEDS_SHARP_STORE");
}

TEST(KeepStoreRuleTests, formalReadOnlyCompiles) {
    expectCompiles(
        "public final class K {\n"
        "    int64 n;\n"
        "    public K() { }\n"
        "    public void read(Cell p) { this.n = p.n; }\n"
        "}\n");
}

// 4.2.1
TEST(KeepStoreRuleTests, heapLocalPlainStoreRejected) {
    std::string msg = expectError(
        "public final class K {\n"
        "    Cell c;\n"
        "    public K() { }\n"
        "    public void fill() { Cell l = heap Cell(1L); this.c = l; }\n"
        "}\n",
        "CAJETA_ERROR_KEEP_NEEDS_SHARP_STORE");
    EXPECT_NE(msg.find("`l`"), std::string::npos) << msg;
}

// 4.1: a local bound from a plain call may hold a title.
TEST(KeepStoreRuleTests, callLocalPlainStoreRejected) {
    expectError(
        "public final class K {\n"
        "    Cell c;\n"
        "    public K() { }\n"
        "    public void fill() { Cell l = D.plainMake(); this.c = l; }\n"
        "}\n",
        "CAJETA_ERROR_KEEP_NEEDS_SHARP_STORE");
}

// 4.2.2 and rule 6: `#=` of a local, and a producer written in place, compile.
TEST(KeepStoreRuleTests, ownedLocalSharpStoreAndInPlaceProducerCompile) {
    expectCompiles(
        "public final class K {\n"
        "    Cell c;\n"
        "    Cell d;\n"
        "    public K() { }\n"
        "    public void fill() {\n"
        "        Cell l = heap Cell(1L);\n"
        "        this.c #= l;\n"
        "        this.d = heap Cell(2L);\n"
        "    }\n"
        "}\n");
}

// 4.2.3
TEST(KeepStoreRuleTests, borrowedLocalPlainStoreCompiles) {
    expectCompiles(
        "public final class K {\n"
        "    Cell c;\n"
        "    public K() { }\n"
        "    public void set(^Cell p) { Cell l = p; this.c = l; }\n"
        "}\n");
}

// 4.2.4
TEST(KeepStoreRuleTests, interiorReadOfPlainFormalRejected) {
    std::string msg = expectError(
        "public final class K {\n"
        "    Cell c;\n"
        "    public K() { }\n"
        "    public void set(Node n) { this.c = n.c; }\n"
        "}\n",
        "CAJETA_ERROR_INTERIOR_KEEP_NEEDS_BORROW_PARAM");
    EXPECT_NE(msg.find("`n`"), std::string::npos) << msg;
}

// 4.2.4 fixed with `^`, and spec 9.4: an interior `#=` records the slot's title.
TEST(KeepStoreRuleTests, interiorReadThroughBorrowFormalOrSharpStoreCompiles) {
    expectCompiles(
        "public final class K {\n"
        "    Cell c;\n"
        "    Cell d;\n"
        "    public K() { }\n"
        "    public void set(^Node n) { this.c = n.c; }\n"
        "    public void take(#Node n) { this.d #= n.c; }\n"
        "}\n");
}

// 4.2.5
TEST(KeepStoreRuleTests, storeIntoNonEscapingHolderLocalCompiles) {
    expectCompiles(
        "public final class K {\n"
        "    public K() { }\n"
        "    public static int64 peek(Cell p) {\n"
        "        Node h = heap Node();\n"
        "        h.c = p;\n"
        "        return h.c.n;\n"
        "    }\n"
        "}\n");
}

// plan 3.2.7: a diagnostic inside an instantiated template names the template's own line.
TEST(KeepStoreRuleTests, errorInsideTemplateNamesItsRealLine) {
    std::string src =
        "package test;\n"                                   // 1
        "import cajeta.lang.System;\n"                      // 2
        "public class Cell {\n"                             // 3
        "    public int64 n;\n"                             // 4
        "    public Cell(int64 v) { this.n = v; }\n"        // 5
        "}\n"                                               // 6
        "public final class Slot<T> {\n"                    // 7
        "    T v;\n"                                        // 8
        "    public Slot() { }\n"                           // 9
        "    public void put(T p) { this.v = p; }\n"        // 10
        "}\n"                                               // 11
        "public final class D {\n"
        "    public static int64 run() {\n"
        "        Slot<Cell> s = heap Slot<Cell>();\n"
        "        return 0L;\n"
        "    }\n"
        "}\n";
    int line = -1;
    try {
        CajetaJit::compile(src, "test.D");
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_KEEP_NEEDS_SHARP_STORE") << e.getMessage();
        line = e.getLine();
    }
    EXPECT_EQ(line, 10);
}
