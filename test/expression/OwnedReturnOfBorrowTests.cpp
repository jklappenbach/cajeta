// stdlib-ownership-convention 8.2.6 (spec §4.5) —
// `CAJETA_ERROR_OWNED_RETURN_OF_BORROW`: a `#` return is a contract the CALLEE
// must keep, so every return must establish a title.
//
// Nothing checks this today. A plain return under a `#` declaration takes the
// STATIC mode — flag 1, asserted unconditionally — and the existing TITLE_MISS
// guard only covers `return #x` with a runtime flag. That gap is what let
// `ParallelDriver.reduceParallelChain` declare `#T`, return its accumulator
// plain, and hand a caller a forged title over the seed it had only lent
// (8.4.2). A canary test found it; the compiler should have.
//
// The check reuses provenance the earlier units already record on the Field —
// U2's `callBorrowOrigin` and U3's `paramBorrowOrigin` — and splits with Unit 2
// by FORM: `return #x` is Unit 2's (MOVE_OF_BORROW, with the runtime TITLE_MISS
// behind it), while a PLAIN return under a `#` declaration was diagnosed by
// nothing at all. The tests below pin that boundary in both directions.
//
// It fires only when the frame is KNOWN to hold a borrow, never on "cannot
// prove" — spec §7.2's discipline, and the reason it should not repeat Unit
// 3's gate breakage.
//
// `return #= x` stays legal throughout: it declares the return carries the
// mode rather than claiming a title, and it is the sanctioned fix for exactly
// this situation (8.4.2 uses it).

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

#include "cajeta/error/Exception.h"

using cajeta_test::CajetaJit;

namespace {

const char* kPrelude =
    "package test;\n"
    "public class Cell {\n"
    "    public int32 n;\n"
    "    public Cell(int32 nn) { this.n = nn; }\n"
    "}\n"
    "public class Bank {\n"
    "    public Cell c;\n"
    "    public Bank(#Cell v) { this.c #= v; }\n"
    "    public Cell get() { return this.c; }\n"
    "}\n";

void expectRejected(const std::string& src, const std::string& code) {
    try {
        CajetaJit::compile(src, "test.D");
        ADD_FAILURE() << "expected " << code;
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), code)
            << "wrong diagnostic: " << e.getMessage();
    } catch (const std::exception& e) {
        ADD_FAILURE() << "wrong exception type: " << e.what();
    }
}

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

}  // namespace

// The 8.4.2 shape, reduced: `T acc = seed;` from a PLAIN parameter, returned
// under a `#` declaration. The frame never held a title — the caller lent one —
// so the `#` promise cannot be kept, and on the path where `acc` is never
// reassigned the caller is handed a forged title over its own value.
TEST(OwnedReturnOfBorrowTests, returnOfLocalFromPlainParamRejected) {
    expectRejected(std::string(kPrelude) +
        "public final class D {\n"
        "    public static #Cell take(Cell seed) {\n"
        "        Cell acc = seed;\n"
        "        return acc;\n"
        "    }\n"
        "    public static int32 run() { return 0; }\n"
        "}\n", "CAJETA_ERROR_OWNED_RETURN_OF_BORROW");
}

// The other provenance, same PLAIN-return form: `x` came from a borrow-returning
// call, so the receiver still owns and frees the value.
//
// This test is why the check covers both origins. Read against the first
// baseline it looked already-diagnosed — gtest interleaves failure text, and
// the `MOVE_OF_BORROW` message printed between two `[ FAILED ]` lines belonged
// to the `return #x` case below, not to this one. Narrowing on that misreading
// made this shape compile clean, and the next run caught it. Attribute
// interleaved output by running the cases apart, not by eye.
TEST(OwnedReturnOfBorrowTests, returnOfLocalFromBorrowingCallRejected) {
    expectRejected(std::string(kPrelude) +
        "public final class D {\n"
        "    public static #Cell take(Bank b) {\n"
        "        Cell x = b.get();\n"
        "        return x;\n"
        "    }\n"
        "    public static int32 run() { return 0; }\n"
        "}\n", "CAJETA_ERROR_OWNED_RETURN_OF_BORROW");
}

// Same shape spelled `return #x` — also Unit 2's, and squarely so: this is the
// literal `#`-on-a-borrow the MOVE_OF_BORROW check was built for.
TEST(OwnedReturnOfBorrowTests, moveReturnOfBorrowedLocalIsUnit2s) {
    expectRejected(std::string(kPrelude) +
        "public final class D {\n"
        "    public static #Cell take(Bank b) {\n"
        "        Cell x = b.get();\n"
        "        return #x;\n"
        "    }\n"
        "    public static int32 run() { return 0; }\n"
        "}\n", "CAJETA_ERROR_MOVE_OF_BORROW");
}

// Everything the check must NOT touch, in one program — a compile costs ~40s
// and these are all "still legal" controls. A check with no passing controls is
// a check nobody can trust (CLAUDE.md §5).
//
//   fresh()      a `heap` allocation establishes a title outright
//   viaLocal()   a local holding a fresh allocation — `collectParallelChain`'s
//                shape, whose `#R` claim IS true on every path
//   viaCall()    a `#`-returning call result
//   modeCarry()  `return #= a` — the sanctioned escape, and 8.4.2's own fix
//   ownedParam() a `#T` parameter is statically owned
TEST(OwnedReturnOfBorrowTests, legitimateOwnedReturnsStillCompile) {
    std::string src = std::string(kPrelude) +
        "public final class D {\n"
        "    public static #Cell fresh() { return heap Cell(1); }\n"
        "    public static #Cell viaLocal() {\n"
        "        Cell x = heap Cell(2);\n"
        "        return x;\n"
        "    }\n"
        "    public static #Cell viaCall() { return D.fresh(); }\n"
        "    public static #Cell modeCarry(Cell p) {\n"
        "        Cell a #= p;\n"
        "        return #= a;\n"
        "    }\n"
        "    public static #Cell ownedParam(#Cell p) { return p; }\n"
        "    public static int32 run() {\n"
        "        int32 t = 0;\n"
        "        {\n"
        "            Cell a = D.fresh();\n"
        "            Cell b = D.viaLocal();\n"
        "            Cell c = D.viaCall();\n"
        "            Cell d = heap Cell(4);\n"
        "            Cell e = D.modeCarry(d);\n"
        "            Cell f = heap Cell(5);\n"
        "            Cell g = D.ownedParam(#f);\n"
        "            t = a.n + b.n + c.n + e.n + g.n;\n"
        "        }\n"
        "        return t;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1 + 2 + 1 + 4 + 5);
}
