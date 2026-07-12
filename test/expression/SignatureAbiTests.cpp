//
// title-tracking Unit 5 (plan 5.1) — signature transfer ABI (spec §4).
// Formals: `V v` borrows (call-site `#` rejected), `#V v` demands a title
// (plain arg rejected; callee owns — drops, stores, or forwards it), and
// `#?V v` accepts both spellings with a hidden per-call i1 flag (§4.4)
// that threads into field bits, caller drop entries, and forwarded calls.
// Returns mirror the formals: `#V` statically owned, `#?V` flag-carried.
// Mode-only overloads are impossible (dispatch is mode-erased) and are
// rejected at declaration (§4.3.1).
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

#include "cajeta/error/Exception.h"

using cajeta_test::CajetaJit;

namespace {

const char* kCellSrc =
    "package test;\n"
    "public class Cell {\n"
    "    public int32 n;\n"
    "    public Cell(int32 nn) { this.n = nn; }\n"
    "}\n";

int32_t runI32(const std::string& src, const char* entryClass = "test.D") {
    auto jit = CajetaJit::compile(src, entryClass);
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

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

} // namespace

// 5.1.1a — a `#V` formal demands a surrendered title: a plain (borrowed)
// argument is TRANSFER_REQUIRED at the call edge.
TEST(SignatureAbiTests, sharpFormalPlainArgRejected) {
    std::string src = std::string(kCellSrc) +
        "public class Sink {\n"
        "    public int32 seen;\n"
        "    public void take(#Cell c) {\n"
        "        this.seen = c.n;\n"
        "        Cajeta.dropValue(c);\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Sink s = heap Sink();\n"
        "        Cell v = heap Cell(3);\n"
        "        s.take(v);\n"              // plain arg into #Cell — reject
        "        return s.seen;\n"
        "    }\n"
        "}\n";
    std::string msg =
        compileExpectError(src, "CAJETA_ERROR_TRANSFER_REQUIRED");
    EXPECT_NE(msg.find("take"), std::string::npos) << msg;
}

// 5.1.1b — `#x` and an owned rvalue (fresh construction) both satisfy a
// `#V` formal; the callee owns. Explicit dropValue keeps this probe
// independent of 5.2.5 (the automatic callee-side drop).
TEST(SignatureAbiTests, sharpFormalAcceptsMoveAndOwnedRvalue) {
    std::string src = std::string(kCellSrc) +
        "public class Sink {\n"
        "    public int32 seen;\n"
        "    public void take(#Cell c) {\n"
        "        this.seen = this.seen + c.n;\n"
        "        Cajeta.dropValue(c);\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 work() {\n"
        "        Sink s = heap Sink();\n"
        "        Cell v = heap Cell(4);\n"
        "        s.take(#v);\n"             // moved local
        "        s.take(heap Cell(2));\n"   // owned rvalue (§4.1.2)
        "        return s.seen;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int32 t = work();\n"
        "        int64 leaked = Cajeta.liveCount() - base;\n"
        "        return (int32) (leaked * 100) + t;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 6);
}

// 5.1.1c — the body of a `#V` callee owns its formal and may forward it
// to another `#V` callee; exactly one drop total (the forward deactivates
// the origin's ownership — no double free, no leak).
TEST(SignatureAbiTests, sharpFormalForwardTransfers) {
    std::string src = std::string(kCellSrc) +
        "public class Inner {\n"
        "    public int32 seen;\n"
        "    public void take2(#Cell c) {\n"
        "        this.seen = c.n;\n"
        "        Cajeta.dropValue(c);\n"
        "    }\n"
        "}\n"
        "public class Outer {\n"
        "    public Inner inner;\n"
        "    public Outer() { this.inner = #heap Inner(); }\n"
        "    public void take(#Cell c) {\n"
        "        this.inner.take2(#c);\n"   // forward the title
        "    }\n"
        "    public int32 seen() { return this.inner.seen; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 work() {\n"
        "        Outer o = heap Outer();\n"
        "        o.take(#heap Cell(5));\n"
        "        return o.seen();\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int32 t = work();\n"
        "        int64 leaked = Cajeta.liveCount() - base;\n"
        "        return (int32) (leaked * 100) + t;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 5);
}

// 5.1.2 — the inverse edge: `#` at the call site into a PLAIN formal is a
// compile error (new — retires the concrete-class moveMask hint, which
// silently accepted and transferred).
TEST(SignatureAbiTests, sharpIntoPlainFormalRejected) {
    std::string src = std::string(kCellSrc) +
        "public class Viewer {\n"
        "    public int32 look(Cell c) { return c.n; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Viewer w = heap Viewer();\n"
        "        Cell v = heap Cell(3);\n"
        "        return w.look(#v);\n"      // # into borrow formal — reject
        "    }\n"
        "}\n";
    std::string msg =
        compileExpectError(src, "CAJETA_ERROR_TRANSFER_NOT_ACCEPTED");
    EXPECT_NE(msg.find("look"), std::string::npos) << msg;
}

// 5.1.3a — `#?V` formal, owned spelling: the hidden flag rides in true,
// the store into the field propagates it into the field's ownership bit,
// and the Box's teardown drops the element. Net leak 0.
TEST(SignatureAbiTests, maybeFormalOwnedSpellingFieldOwns) {
    std::string src = std::string(kCellSrc) +
        "public class Box {\n"
        "    public Cell c;\n"
        "    public void put(#?Cell v) { this.c = #v; }\n"
        "    public int32 peek() { return this.c.n; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 work() {\n"
        "        Box b = heap Box();\n"
        "        b.put(#heap Cell(4));\n"   // flag true → field bit owned
        "        return b.peek();\n"
        "    }\n"                           // Box teardown drops the Cell
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int32 t = work();\n"
        "        int64 leaked = Cajeta.liveCount() - base;\n"
        "        return (int32) (leaked * 100) + t;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 4);
}

// 5.1.3b — `#?V` formal, plain spelling: flag false, the field records a
// borrow, Box teardown skips it, and the caller's local keeps its single
// drop. The source outlives the Box and stays readable.
TEST(SignatureAbiTests, maybeFormalPlainSpellingFieldBorrows) {
    std::string src = std::string(kCellSrc) +
        "public class Box {\n"
        "    public Cell c;\n"
        "    public void put(#?Cell v) { this.c = #v; }\n"
        "    public int32 peek() { return this.c.n; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 work() {\n"
        "        Cell mine = heap Cell(9);\n"
        "        {\n"
        "            Box b = heap Box();\n"
        "            b.put(mine);\n"        // flag false → field bit borrow
        "            if (b.peek() != 9) { return -98; }\n"
        "        }\n"                       // Box drops; must NOT free mine
        "        return mine.n;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int32 t = work();\n"
        "        int64 leaked = Cajeta.liveCount() - base;\n"
        "        return (int32) (leaked * 100) + t;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 9);
}

// 5.1.4a — `#?V` return, flag true: the caller's drop entry is armed by
// the returned flag; the local drops at scope exit. Net leak 0.
TEST(SignatureAbiTests, maybeReturnOwnedArmsCallerDrop) {
    std::string src = std::string(kCellSrc) +
        "public class Echo {\n"
        "    public #?Cell pass(#?Cell v) { return #v; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 work() {\n"
        "        Echo e = heap Echo();\n"
        "        Cell r = e.pass(#heap Cell(6));\n"  // flag true → r owns
        "        return r.n;\n"
        "    }\n"                                    // r drops here
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int32 t = work();\n"
        "        int64 leaked = Cajeta.liveCount() - base;\n"
        "        return (int32) (leaked * 100) + t;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 6);
}

// 5.1.4b — `#?V` return, flag false: the caller's entry stays inactive;
// the original owner keeps the single drop and the value survives the
// callee round-trip.
TEST(SignatureAbiTests, maybeReturnBorrowedLeavesCallerInactive) {
    std::string src = std::string(kCellSrc) +
        "public class Echo {\n"
        "    public #?Cell pass(#?Cell v) { return #v; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 work() {\n"
        "        Cell mine = heap Cell(7);\n"
        "        Echo e = heap Echo();\n"
        "        {\n"
        "            Cell r = e.pass(mine);\n"  // flag false → r borrows
        "            if (r.n != 7) { return -97; }\n"
        "        }\n"                           // r's exit must NOT free mine
        "        return mine.n;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int32 t = work();\n"
        "        int64 leaked = Cajeta.liveCount() - base;\n"
        "        return (int32) (leaked * 100) + t;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

// 5.1.5 — overloads distinguished only by transfer mode are impossible
// (dispatch is mode-erased): rejected at declaration, not at a call site.
TEST(SignatureAbiTests, modeOnlyOverloadRejectedAtDeclaration) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public void f(Cell c) { }\n"
        "    public void f(#Cell c) { Cajeta.dropValue(c); }\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    std::string msg =
        compileExpectError(src, "CAJETA_ERROR_TRANSFER_MODE_OVERLOAD");
    EXPECT_NE(msg.find("f"), std::string::npos) << msg;
}

// 5.1.6 — forwarding a `#?` formal to a `#?` callee threads the per-call
// flag explicitly (the failure mode that killed moveMask: forwarding
// chains lost the answer). Owned path: the innermost store owns and the
// chain leaks nothing. Borrowed path: the source survives the chain.
TEST(SignatureAbiTests, maybeFormalForwardThreadsFlag) {
    std::string src = std::string(kCellSrc) +
        "public class Box {\n"
        "    public Cell c;\n"
        "    public void inner(#?Cell v) { this.c = #v; }\n"
        "    public void outer(#?Cell v) { this.inner(#v); }\n"
        "    public int32 peek() { return this.c.n; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 work() {\n"
        "        int32 t = 0;\n"
        "        {\n"
        "            Box b = heap Box();\n"
        "            b.outer(#heap Cell(3));\n"     // true threads through
        "            t = t + b.peek();\n"
        "        }\n"                               // owned → Box drops it
        "        Cell mine = heap Cell(4);\n"
        "        {\n"
        "            Box b2 = heap Box();\n"
        "            b2.outer(mine);\n"             // false threads through
        "            if (b2.peek() != 4) { return -96; }\n"
        "        }\n"                               // borrow → mine survives
        "        return t + mine.n;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int32 t = work();\n"
        "        int64 leaked = Cajeta.liveCount() - base;\n"
        "        return (int32) (leaked * 100) + t;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

// 5.1.7 (drives 5.2.5) — a class-typed `#V` formal that the body neither
// forwards nor stores is CONSUMED: the callee drops it via its own entry.
// No Cajeta.dropValue in sight (the ~HashMap idiom this retires). Today
// this leaks — the callee-side entry does not exist yet.
TEST(SignatureAbiTests, sharpFormalConsumedDropsInCallee) {
    std::string src = std::string(kCellSrc) +
        "public class Sink {\n"
        "    public int32 seen;\n"
        "    public void take(#Cell c) {\n"
        "        this.seen = this.seen + c.n;\n"
        "    }\n"                           // c consumed → callee drops it
        "}\n"
        "public final class D {\n"
        "    public static int32 work() {\n"
        "        Sink s = heap Sink();\n"
        "        s.take(#heap Cell(2));\n"
        "        s.take(#heap Cell(3));\n"
        "        return s.seen;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int32 t = work();\n"
        "        int64 leaked = Cajeta.liveCount() - base;\n"
        "        return (int32) (leaked * 100) + t;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 5);
}
