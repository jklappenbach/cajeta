// field-store-ownership Unit 2 (spec 2, 9.2, 9.3): `^T` formals are borrow-only.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

#include "cajeta/error/Exception.h"

using cajeta_test::CajetaJit;

namespace {

const char* kCell =
    "package test;\n"
    "public class Cell {\n"
    "    public int64 n;\n"
    "    public Cell(int64 v) { this.n = v; }\n"
    "}\n"
    "public final class Keep {\n"
    "    public Cell c;\n"
    "    public Keep() { }\n"
    "    public void keep(^Cell p) { this.c = p; }\n"
    "}\n";

int64_t runI64(const std::string& body) {
    auto jit = CajetaJit::compile(std::string(kCell) + body, "test.D");
    auto fn = jit->lookup<int64_t (*)()>("run");
    return fn();
}

std::string expectError(const std::string& body, const std::string& code) {
    try {
        CajetaJit::compile(std::string(kCell) + body, "test.D");
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

}  // namespace

// 2.2.1 and 3.2.1: a lend into a `^` formal kept with `=` compiles and reads back.
TEST(BorrowOnlyFormalTests, lendIntoBorrowFormalKeptWithPlainStore) {
    EXPECT_EQ(runI64(
        "public final class D {\n"
        "    public static int64 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int64 got = 0L;\n"
        "        {\n"
        "            Cell a = heap Cell(8100L);\n"
        "            Keep k = heap Keep();\n"
        "            k.keep(a);\n"
        "            got = k.c.n;\n"
        "        }\n"
        "        return got * 10L + (Cajeta.liveCount() - base);\n"
        "    }\n"
        "}\n"),
        81000);
}

// 2.2.2
TEST(BorrowOnlyFormalTests, transferIntoBorrowFormalRejected) {
    std::string msg = expectError(
        "public final class D {\n"
        "    public static int64 run() {\n"
        "        Cell a = heap Cell(1L);\n"
        "        Keep k = heap Keep();\n"
        "        k.keep(#a);\n"
        "        return 0L;\n"
        "    }\n"
        "}\n",
        "CAJETA_ERROR_TRANSFER_INTO_BORROW_PARAM");
    EXPECT_NE(msg.find("`p`"), std::string::npos) << msg;
}

// 2.2.3
TEST(BorrowOnlyFormalTests, freshTemporaryIntoBorrowFormalRejected) {
    expectError(
        "public final class D {\n"
        "    public static int64 run() {\n"
        "        Keep k = heap Keep();\n"
        "        k.keep(heap Cell(1L));\n"
        "        return 0L;\n"
        "    }\n"
        "}\n",
        "CAJETA_ERROR_TRANSFER_INTO_BORROW_PARAM");
}

// 2.2.3, constructor form.
TEST(BorrowOnlyFormalTests, transferIntoBorrowConstructorFormalRejected) {
    expectError(
        "public final class View {\n"
        "    Cell c;\n"
        "    public View(^Cell c) { this.c = c; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int64 run() {\n"
        "        Cell a = heap Cell(1L);\n"
        "        View v = heap View(#a);\n"
        "        return 0L;\n"
        "    }\n"
        "}\n",
        "CAJETA_ERROR_TRANSFER_INTO_BORROW_PARAM");
}

// 2.2.4
TEST(BorrowOnlyFormalTests, moveOfBorrowFormalRejected) {
    expectError(
        "public final class D {\n"
        "    static void sink(#Cell c) { }\n"
        "    static void pass(^Cell c) { D.sink(#c); }\n"
        "    public static int64 run() { return 0L; }\n"
        "}\n",
        "CAJETA_ERROR_MOVE_OF_BORROW");
}

// 2.2.5: passed on plainly and stored with `#=`, it stays a borrow and the lender frees.
TEST(BorrowOnlyFormalTests, borrowFormalForwardedAndSharpStoredStaysBorrow) {
    EXPECT_EQ(runI64(
        "public final class Sink {\n"
        "    public Cell c;\n"
        "    public Sink() { }\n"
        "    public void put(Cell p) { this.c #= p; }\n"
        "}\n"
        "public final class D {\n"
        "    static void relay(Sink s, ^Cell p) { s.put(p); }\n"
        "    static void store(Sink s, ^Cell p) { s.c #= p; }\n"
        "    public static int64 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int64 got = 0L;\n"
        "        {\n"
        "            Cell a = heap Cell(8100L);\n"
        "            Sink s = heap Sink();\n"
        "            Sink t = heap Sink();\n"
        "            D.relay(s, a);\n"
        "            D.store(t, a);\n"
        "            got = s.c.n + t.c.n;\n"
        "        }\n"
        "        return got * 10L + (Cajeta.liveCount() - base);\n"
        "    }\n"
        "}\n"),
        162000);
}

// 2.2.6, class override.
TEST(BorrowOnlyFormalTests, overrideWithDifferentMarkRejected) {
    expectError(
        "public class Base {\n"
        "    public Base() { }\n"
        "    public void take(^Cell c) { }\n"
        "}\n"
        "public class Derived extends Base {\n"
        "    public Derived() { }\n"
        "    public void take(Cell c) { }\n"
        "}\n"
        "public final class D {\n"
        "    public static int64 run() { Derived d = heap Derived(); return 0L; }\n"
        "}\n",
        "CAJETA_ERROR_BORROW_MARK_MISMATCH");
}

// 2.2.6, interface implementation.
TEST(BorrowOnlyFormalTests, implementationWithDifferentMarkRejected) {
    expectError(
        "public interface Taker {\n"
        "    void take(^Cell c);\n"
        "}\n"
        "public class Impl implements Taker {\n"
        "    public Impl() { }\n"
        "    public void take(Cell c) { }\n"
        "}\n"
        "public final class D {\n"
        "    public static int64 run() { Impl i = heap Impl(); return 0L; }\n"
        "}\n",
        "CAJETA_ERROR_BORROW_MARK_MISMATCH");
}

// 2.2.7
TEST(BorrowOnlyFormalTests, transferThroughInterfaceRejected) {
    expectError(
        "public interface Taker {\n"
        "    void take(^Cell c);\n"
        "}\n"
        "public class Impl implements Taker {\n"
        "    public Impl() { }\n"
        "    public void take(^Cell c) { }\n"
        "}\n"
        "public final class D {\n"
        "    public static int64 run() {\n"
        "        Taker t = heap Impl();\n"
        "        Cell a = heap Cell(1L);\n"
        "        t.take(#a);\n"
        "        return 0L;\n"
        "    }\n"
        "}\n",
        "CAJETA_ERROR_TRANSFER_INTO_BORROW_PARAM");
}

// 2.2.8: a function-typed `^` formal kept with `=`.
TEST(BorrowOnlyFormalTests, functionTypedBorrowFormalKept) {
    EXPECT_EQ(runI64(
        "public final class Hold {\n"
        "    (int64) -> int64 f;\n"
        "    public Hold() { }\n"
        "    public void keep(^(int64) -> int64 g) { this.f = g; }\n"
        "    public int64 call(int64 x) { return this.f(x); }\n"
        "}\n"
        "public final class D {\n"
        "    public static int64 run() {\n"
        "        (int64) -> int64 twice = (int64 x) -> x * 2L;\n"
        "        Hold h = heap Hold();\n"
        "        h.keep(twice);\n"
        "        return h.call(4050L);\n"
        "    }\n"
        "}\n"),
        8100);
}

// 2.2.8: a lambda formal may be `^`.
TEST(BorrowOnlyFormalTests, lambdaBorrowFormalCompiles) {
    EXPECT_EQ(runI64(
        "public final class D {\n"
        "    public static int64 run() {\n"
        "        (Cell) -> int64 get = (^Cell c) -> c.n;\n"
        "        Cell a = heap Cell(8100L);\n"
        "        return get(a);\n"
        "    }\n"
        "}\n"),
        8100);
}

// 9.2
TEST(BorrowOnlyFormalTests, borrowMarkOnPrimitiveRejected) {
    expectError(
        "public final class D {\n"
        "    static int64 id(^int64 n) { return n; }\n"
        "    public static int64 run() { return D.id(1L); }\n"
        "}\n",
        "CAJETA_ERROR_BORROW_MARK_ON_VALUE");
}

// 9.3: `^K` on a template parameter is legal and ignored for a primitive.
TEST(BorrowOnlyFormalTests, borrowMarkOnTemplateParameterFormal) {
    EXPECT_EQ(runI64(
        "public final class Slot<K> {\n"
        "    K k;\n"
        "    public Slot() { }\n"
        "    public void put(^K key) { this.k = key; }\n"
        "    public K get() { return this.k; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int64 run() {\n"
        "        Slot<int64> p = heap Slot<int64>();\n"
        "        p.put(8000L);\n"
        "        Cell a = heap Cell(100L);\n"
        "        Slot<Cell> q = heap Slot<Cell>();\n"
        "        q.put(a);\n"
        "        return p.get() + q.get().n;\n"
        "    }\n"
        "}\n"),
        8100);
}

// 9.3: on a class instantiation the mark refuses a transfer.
TEST(BorrowOnlyFormalTests, borrowMarkOnTemplateParameterRefusesTransfer) {
    expectError(
        "public final class Slot<K> {\n"
        "    K k;\n"
        "    public Slot() { }\n"
        "    public void put(^K key) { this.k = key; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int64 run() {\n"
        "        Cell a = heap Cell(1L);\n"
        "        Slot<Cell> q = heap Slot<Cell>();\n"
        "        q.put(#a);\n"
        "        return 0L;\n"
        "    }\n"
        "}\n",
        "CAJETA_ERROR_TRANSFER_INTO_BORROW_PARAM");
}

TEST(BorrowOnlyFormalTests, borrowMarkOnVarargsRejected) {
    expectError(
        "public final class D {\n"
        "    static int64 count(^Cell... cs) { return 0L; }\n"
        "    public static int64 run() { return 0L; }\n"
        "}\n",
        "CAJETA_ERROR_BORROW_MARK_ON_VARARGS");
}

TEST(BorrowOnlyFormalTests, overloadDifferingOnlyInBorrowMarkRejected) {
    expectError(
        "public final class D {\n"
        "    static void f(^Cell c) { }\n"
        "    static void f(Cell c) { }\n"
        "    public static int64 run() { return 0L; }\n"
        "}\n",
        "CAJETA_ERROR_TRANSFER_MODE_OVERLOAD");
}

// 2.2.3: an owned `#T` call result is a temporary too.
TEST(BorrowOnlyFormalTests, ownedCallResultIntoBorrowFormalRejected) {
    expectError(
        "public final class D {\n"
        "    static #Cell fresh() { return heap Cell(1L); }\n"
        "    public static int64 run() {\n"
        "        Keep k = heap Keep();\n"
        "        k.keep(D.fresh());\n"
        "        return 0L;\n"
        "    }\n"
        "}\n",
        "CAJETA_ERROR_TRANSFER_INTO_BORROW_PARAM");
}
