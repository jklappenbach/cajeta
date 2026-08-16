// stdlib-ownership-convention Unit 2 (spec 3.1, 4.1, 4.3) — transfer of
// a borrow, reported as the EXISTING CAJETA_ERROR_MOVE_OF_BORROW.
//
// The spec proposed a new code; the compiler already had one for exactly
// this defect, and its own comment recorded the gap being closed here:
// "a call-result local (`conn = next.get()`) stays unchecked until the
// `#?` runtime-owner ABI can carry its role". No runtime ABI is needed —
// the callee's DECLARED return spelling is static truth, and
// LocalVariableDeclaration already computes it to decide the local's
// drop entry. Only the provenance was being discarded.
//
// `#x` surrenders title. When `x` holds a BORROW returned by a
// plain (non-`#`) method, the surrender is a lie and the receiver frees
// memory the callee's object still owns. Mirror of
// CAJETA_ERROR_DANGLING_LEND, which catches the other direction.
//
// Plain FORMALS are deliberately NOT covered: their ownership is fixed
// at the call site and carried at run time by the transfer word, so `#p`
// forwards the arrived mode. See transferOfPlainFormalStillCompiles.
//
// Motivating case, from cajeta-llama Unit 13:
//
//     int8[] kb = o.keyAt(j);              // BORROW of the object's key
//     String key = heap String(#kb, kl);   // String takes title...
//                                          // ...and frees what `o` frees
//
// It compiled, and the corruption surfaced as garbage bytes in unrelated
// output several frames away. Three of that unit's four ownership bugs
// were of this shape; only the one the compiler already checks was caught
// at build time.
//
// RED until 2.2.1 lands.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

#include "cajeta/error/Exception.h"

using cajeta_test::CajetaJit;

namespace {

// A container that hands out a BORROW of its interior (the `keyAt` shape)
// and, separately, an OWNED copy (the `toBytes` shape). The two differ
// only in the `#` on the return type — which is exactly the signal the
// check reads.
const char* kBoxSrc =
    "package test;\n"
    "public final class Box {\n"
    "    int8[] data;\n"
    "    public Box() {\n"
    "        this.data = heap int8[4];\n"
    "        this.data[0] = (int8) 7;\n"
    "    }\n"
    "    public int8[] borrowData() { return this.data; }\n"
    "    public #int8[] copy() {\n"
    "        int8[] out = heap int8[4];\n"
    "        int32 i = 0;\n"
    "        while (i < 4) { out[i] = this.data[i]; i = i + 1; }\n"
    "        return #out;\n"
    "    }\n"
    "}\n"
    "public final class Sink {\n"
    "    int8[] held;\n"
    "    public Sink(#int8[] b) { this.held #= b; }\n"
    "    public int32 first() { return (int32) this.held[0]; }\n"
    "}\n";

std::string compileExpectError(const std::string& src,
                               const std::string& expectCode) {
    try {
        CajetaJit::compile(src, "test.D");
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), expectCode);
        return e.getMessage();
    } catch (const std::exception& e) {
        return e.what();
    }
    ADD_FAILURE() << "expected a compile error";
    return "";
}

void compileExpectOk(const std::string& src) {
    try {
        CajetaJit::compile(src, "test.D");
    } catch (cajeta::Exception& e) {
        ADD_FAILURE() << "expected a clean compile, got " << e.getErrorId()
                      << ": " << e.getMessage();
    } catch (const std::exception& e) {
        ADD_FAILURE() << "expected a clean compile, got " << e.what();
    }
}

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

}  // namespace

// 2.1.1 — `#x` where x came from a BORROW-returning call is rejected, and
// the message names both the use and the declaration that made the borrow.
TEST(TransferOfBorrowTests, transferOfBorrowReturnRejected) {
    std::string src = std::string(kBoxSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Box b = heap Box();\n"
        "        int8[] v = b.borrowData();\n"       // borrow of b's interior
        "        Sink s = heap Sink(#v);\n"    // ...surrendered: a lie
        "        return s.first();\n"
        "    }\n"
        "}\n";
    std::string msg = compileExpectError(src,
        "CAJETA_ERROR_MOVE_OF_BORROW");
    // 4.3 — name both ends: the offending use and the borrow's origin.
    EXPECT_NE(msg.find("`v`"), std::string::npos)
        << "message should name the local: " << msg;
    EXPECT_NE(msg.find("borrowData"), std::string::npos)
        << "message should name the borrow origin: " << msg;
}

// 2.1.2 — a plain (non-`#`) FORMAL is NOT statically a borrow, and `#p`
// on one must keep compiling.
//
// The spec's first draft called for rejecting this. Reading the compiler
// corrected it: a formal's ownership is fixed at the CALL SITE and
// carried at run time by the transfer word — `f(x)` lends, `f(#x)`
// transfers — so `#p` forwards whatever mode actually arrived
// (conditional acquisition), and `#=` records the forwarded mode per
// slot. Rejecting it statically would break that design and outlaw
// every mode-forwarding wrapper. The existing MOVE_OF_BORROW check
// excludes formals deliberately and correctly; this test pins that.
TEST(TransferOfBorrowTests, transferOfPlainFormalStillCompiles) {
    std::string src = std::string(kBoxSrc) +
        "public final class D {\n"
        "    static int32 keep(int8[] p) {\n"
        "        Sink s = heap Sink(#p);\n"    // forwards the arrived mode
        "        return s.first();\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int8[] a = heap int8[4];\n"
        "        a[0] = (int8) 7;\n"
        "        return D.keep(#a);\n"         // caller transfers
        "    }\n"
        "}\n";
    compileExpectOk(src);
    EXPECT_EQ(runI32(src), 7);
}

// 2.1.3 — the three shapes that DO hold title still compile and run: a
// `heap` allocation, a `#`-returning call, and a `#T` parameter.
TEST(TransferOfBorrowTests, transferOfOwnedStillCompiles) {
    std::string heapSrc = std::string(kBoxSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int8[] a = heap int8[4];\n"
        "        a[0] = (int8) 7;\n"
        "        Sink s = heap Sink(#a);\n"
        "        return s.first();\n"
        "    }\n"
        "}\n";
    compileExpectOk(heapSrc);
    EXPECT_EQ(runI32(heapSrc), 7);

    std::string callSrc = std::string(kBoxSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Box b = heap Box();\n"
        "        int8[] c #= b.copy();\n"       // `#int8[]` return: owned
        "        Sink s = heap Sink(#c);\n"
        "        return s.first();\n"
        "    }\n"
        "}\n";
    compileExpectOk(callSrc);
    EXPECT_EQ(runI32(callSrc), 7);

    std::string paramSrc = std::string(kBoxSrc) +
        "public final class D {\n"
        "    static int32 keep(#int8[] p) {\n"
        "        Sink s = heap Sink(#p);\n"    // `#T` param holds title
        "        return s.first();\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int8[] a = heap int8[4];\n"
        "        a[0] = (int8) 7;\n"
        "        return D.keep(#a);\n"
        "    }\n"
        "}\n";
    compileExpectOk(paramSrc);
    EXPECT_EQ(runI32(paramSrc), 7);
}

// 2.1.4 — the cajeta-llama regression, verbatim in shape: a borrowed
// interior array surrendered to a String constructor. This is the one
// that produced garbage keys in rendered output.
TEST(TransferOfBorrowTests, llamaKeyAtShapeRejected) {
    std::string src =
        "package test;\n"
        "import cajeta.lang.String;\n"
        "public final class Obj {\n"
        "    int8[] key;\n"
        "    public Obj() {\n"
        "        this.key = heap int8[2];\n"
        "        this.key[0] = (int8) 97;\n"
        "        this.key[1] = (int8) 98;\n"
        "    }\n"
        "    public int8[] keyAt() { return this.key; }\n"
        "    public int32 keyLen() { return 2; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Obj o = heap Obj();\n"
        "        int8[] kb = o.keyAt();\n"
        "        String s = heap String(#kb, o.keyLen());\n"
        "        return s.byteLength();\n"
        "    }\n"
        "}\n";
    std::string msg = compileExpectError(src,
        "CAJETA_ERROR_MOVE_OF_BORROW");
    EXPECT_NE(msg.find("`kb`"), std::string::npos)
        << "message should name the local: " << msg;
}

// 2.1.5 — transferring the same local twice: the second `#` has no title
// left to surrender. Already-moved may be diagnosed by the existing
// use-after-move check; either rejection is acceptable, silence is not.
TEST(TransferOfBorrowTests, doubleTransferRejected) {
    std::string src = std::string(kBoxSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int8[] a = heap int8[4];\n"
        "        a[0] = (int8) 7;\n"
        "        Sink s1 = heap Sink(#a);\n"
        "        Sink s2 = heap Sink(#a);\n"   // no title left
        "        return s1.first() + s2.first();\n"
        "    }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.D");
        ADD_FAILURE() << "expected a compile error for the double transfer";
    } catch (cajeta::Exception& e) {
        // Either diagnosis is fine; the point is that it is diagnosed.
        EXPECT_TRUE(e.getErrorId() == "CAJETA_ERROR_TRANSFER_OF_BORROW"
                    || e.getErrorId().find("MOVE") != std::string::npos
                    || e.getErrorId().find("BORROW") != std::string::npos)
            << "unexpected error id: " << e.getErrorId();
    } catch (const std::exception&) {
        // A non-cajeta throw is still a rejection.
    }
}

// A borrow that is COPIED before transfer is the documented fix, and it
// must keep compiling — the check has to leave the escape route open.
TEST(TransferOfBorrowTests, copyThenTransferIsTheFix) {
    std::string src = std::string(kBoxSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Box b = heap Box();\n"
        "        int8[] v = b.borrowData();\n"
        "        int8[] mine = heap int8[4];\n"
        "        int32 i = 0;\n"
        "        while (i < 4) { mine[i] = v[i]; i = i + 1; }\n"
        "        Sink s = heap Sink(#mine);\n"
        "        return s.first();\n"
        "    }\n"
        "}\n";
    compileExpectOk(src);
    EXPECT_EQ(runI32(src), 7);
}

// ---------------------------------------------------------------------
// 2.2.3 — the CALL-ARGUMENT blind spot.
//
// `#x` reaches codegen two different ways. An assignment or a return
// builds a MoveExpression node; a call ARGUMENT instead sets
// MethodCallParameter::callerTransferred and leaves a BARE IDENTIFIER as
// the child, so no MoveExpression is ever constructed. Every borrow check
// lived in MoveExpression::generateCode, so argument position saw none of
// them — a blind spot the PRE-EXISTING checks shared, not one this unit
// introduced. It mattered because argument position is where the
// cajeta-llama corruption actually lived (`heap String(#kb, kl)`).
//
// The two rejection tests above are themselves argument-position cases
// and were red for exactly this reason. These pin the remaining argument
// shapes so a later refactor cannot quietly reopen the gap.
// ---------------------------------------------------------------------

// A borrow surrendered at a plain METHOD-CALL argument (the tests above
// cover the constructor form) is rejected, naming both ends.
TEST(TransferOfBorrowTests, transferAtMethodCallArgumentRejected) {
    std::string src = std::string(kBoxSrc) +
        "public final class D {\n"
        "    static int32 consume(#int8[] p) {\n"
        "        Sink s = heap Sink(#p);\n"
        "        return s.first();\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Box b = heap Box();\n"
        "        int8[] v = b.borrowData();\n"
        "        return D.consume(#v);\n"       // borrow surrendered
        "    }\n"
        "}\n";
    std::string msg = compileExpectError(src, "CAJETA_ERROR_MOVE_OF_BORROW");
    EXPECT_NE(msg.find("`v`"), std::string::npos)
        << "message should name the local: " << msg;
    EXPECT_NE(msg.find("borrowData"), std::string::npos)
        << "message should name the borrow origin: " << msg;
}

// The control the closure must not break: LENDING the same borrow at the
// same argument position is correct and still compiles. Closing the blind
// spot must reject the `#`, not the argument.
TEST(TransferOfBorrowTests, lendAtCallArgumentStillCompiles) {
    std::string src = std::string(kBoxSrc) +
        "public final class D {\n"
        "    static int32 peek(int8[] p) { return (int32) p[0]; }\n"
        "    public static int32 run() {\n"
        "        Box b = heap Box();\n"
        "        int8[] v = b.borrowData();\n"
        "        return D.peek(v);\n"           // lent, not surrendered
        "    }\n"
        "}\n";
    compileExpectOk(src);
    EXPECT_EQ(runI32(src), 7);
}
