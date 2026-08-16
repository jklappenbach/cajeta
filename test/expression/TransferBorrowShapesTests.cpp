//
// The `#T`-formal transfer check, extended to the borrow shapes it silently
// let through (the JsonValue.asString / JsonObject.keys corruption, fixed at
// the call sites in 65588252): CAJETA_ERROR_TRANSFER_REQUIRED fired only for
// bare identifier locals WITH drop entries — a FIELD read (`this.strBytes`),
// an ARRAY-ELEMENT read (`this.keys[i]`), and a BORROW-returning call all
// skipped the check, so a borrow flowed into a consuming formal and, when the
// callee chain ended in a @Native (which can never see the runtime title
// flag), the native freed or adopted memory the caller still owned — reads
// corrupted the JSON tree months after the fact.
//
// The rule pinned here: an argument bound to a `#`-marked formal must be a
// DECLARED transfer — spelled `#x`, or an ownership-producing rvalue (a
// heap/stack creator, a call returning `#R`). Borrow shapes are named,
// located compile errors directing to a copy or an owned local.
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"

#include <string>

using cajeta_test::CajetaJit;

namespace {

std::string wrap(const std::string& members) {
    return
        "package test;\n"
        "public final class T {\n"
        "    static void eat(#int8[] b) { }\n"
        + members +
        "}\n";
}

// "" when the program compiles; the error id otherwise.
std::string errIdOf(const std::string& members) {
    try { CajetaJit::compile(wrap(members), "test.T"); }
    catch (cajeta::Exception& e) { return e.getErrorId(); }
    return "";
}

} // namespace

// A FIELD read is a borrow — the JsonValue.asString shape.
TEST(TransferBorrowShapes, fieldReadIntoTransferFormalRejected) {
    EXPECT_EQ(errIdOf(
        "    int8[] buf;\n"
        "    public T() { this.buf = heap int8[16]; }\n"
        "    public void go() { T.eat(this.buf); }\n"
        "    public static int32 run() {\n"
        "        T t = heap T();\n"
        "        t.go();\n"
        "        return 0;\n"
        "    }\n"), "CAJETA_ERROR_TRANSFER_REQUIRED");
}

// An ARRAY-ELEMENT read is a borrow — the JsonObject.keys shape.
TEST(TransferBorrowShapes, arrayElementIntoTransferFormalRejected) {
    EXPECT_EQ(errIdOf(
        "    public static int32 run() {\n"
        "        int8[][] rows = heap int8[2][];\n"
        "        rows[0] = heap int8[16];\n"
        "        T.eat(rows[0]);\n"
        "        return 0;\n"
        "    }\n"), "CAJETA_ERROR_TRANSFER_REQUIRED");
}

// A call returning a BORROW (no `#R`) cannot satisfy a consuming formal.
TEST(TransferBorrowShapes, borrowReturningCallIntoTransferFormalRejected) {
    EXPECT_EQ(errIdOf(
        "    int8[] buf;\n"
        "    public T() { this.buf = heap int8[16]; }\n"
        "    public int8[] peek() { return this.buf; }\n"
        "    public void go() { T.eat(this.peek()); }\n"
        "    public static int32 run() {\n"
        "        T t = heap T();\n"
        "        t.go();\n"
        "        return 0;\n"
        "    }\n"), "CAJETA_ERROR_TRANSFER_REQUIRED");
}

// An OWNERSHIP-returning call (`-> #R`) is a proven transfer — accepted.
TEST(TransferBorrowShapes, ownedReturningCallStillAccepted) {
    EXPECT_EQ(errIdOf(
        "    static #int8[] mk() {\n"
        "        int8[] b = heap int8[16];\n"
        "        return #b;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        T.eat(T.mk());\n"
        "        return 0;\n"
        "    }\n"), "");
}

// The existing legal spellings stay legal: a surrendered local and a fresh
// creator.
TEST(TransferBorrowShapes, spelledAndFreshArgsStillAccepted) {
    EXPECT_EQ(errIdOf(
        "    public static int32 run() {\n"
        "        int8[] own = heap int8[16];\n"
        "        T.eat(#own);\n"
        "        T.eat(heap int8[8]);\n"
        "        return 0;\n"
        "    }\n"), "");
}

// The real-world regression: an unspelled field read into the consuming
// String byte-ctor (whose @Native core frees/adopts unconditionally) must
// be a compile error, not silent corruption.
TEST(TransferBorrowShapes, stringByteCtorRejectsFieldBorrow) {
    std::string src =
        "package test;\n"
        "import cajeta.lang.String;\n"
        "public final class T {\n"
        "    int8[] bytes;\n"
        "    public T() { this.bytes = heap int8[20]; }\n"
        "    public #String take() { return #heap String(this.bytes, 20); }\n"
        "    public static int32 run() {\n"
        "        T t = heap T();\n"
        "        String s #= t.take();\n"
        "        return s.count();\n"
        "    }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.T");
        FAIL() << "field borrow into the consuming String ctor must not compile";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(std::string(e.getErrorId()), "CAJETA_ERROR_TRANSFER_REQUIRED");
    }
}
