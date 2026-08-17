//
// stdlib-ownership-convention Unit 8 (plan 8.2.8, spec §4.7) — `^T`, the
// VIEW return.
//
// `#T` says "the caller receives a title". Plain `T` says nothing and is
// resolved by per-local provenance at the return statement. `^T` is the third
// stance and the only one that is decidable from the SIGNATURE: the result is
// interior to the receiver, the caller never frees it, and the method emits no
// return-flag write because the flag is statically 0.
//
// The measurement that motivated it (plan 8.2.13, probe 2026-08-17): a `#T`
// that actually hands back a borrow is a live use-after-free — the receipt
// frees the owner's field and the owner's drop doubles it. §4.5's guard misses
// three of the four ways to write it, because it fires only for an
// IdentifierExpression whose borrow provenance was recorded:
//
//     #Cell f(Holder h) { return h.peek(); }   // call result   — missed
//     #Cell f(Cell c)   { return c; }          // formal        — missed
//     #Cell f(Holder h) { return h.c; }        // interior read — missed
//
// Chasing those with more provenance inference is the route §4.7 rejects. With
// `^T` the callee states the fact once and every one of them becomes a lookup,
// which is what `sharpReturnOfViewResultIsRejected` pins.
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

#include "cajeta/error/Exception.h"

using cajeta_test::CajetaJit;

namespace {

// `Holder` owns its `Cell`. Every probe reads `h.c.n` back AFTER the view has
// gone out of scope: 7 means the object is still alive, anything else is
// recycled heap. A wrong title claim shows up as a value, not a crash.
const char* kHolder =
    "package test;\n"
    "public final class Cell {\n"
    "    public int32 n;\n"
    "    public Cell(int32 v) { this.n = v; }\n"
    "}\n"
    "public final class Holder {\n"
    "    public Cell c;\n"
    "    public Holder(int32 v) { this.c #= heap Cell(v); }\n"
    "    /** The view: interior state, the holder still owns and frees it. */\n"
    "    public ^Cell peek() { return this.c; }\n"
    "}\n";

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

// Returns the error id, or "" when the source compiles.
std::string errorOf(const std::string& src) {
    try {
        CajetaJit::compile(src, "test.D");
        return "";
    } catch (cajeta::Exception& e) {
        return e.getErrorId();
    } catch (const std::exception&) {
        return "<non-cajeta-exception>";
    }
}

std::string driver(const std::string& body) {
    return std::string(kHolder) +
        "public final class D {\n"
        "    public static int32 run() {\n" + body +
        "    }\n"
        "}\n";
}

}  // namespace

// ---------------------------------------------------------------- the stance

// The whole point: a `^T` result is a borrow, so the owner's field survives the
// view's scope. This is the probe from 8.2.13 with the signature told the
// truth — the mis-declared `#Cell` form returned recycled heap here.
TEST(BorrowReturn, ViewResultLeavesTheOwnersFieldAlive) {
    EXPECT_EQ(7, runI32(driver(
        "        Holder h #= heap Holder(7);\n"
        "        {\n"
        "            Cell v = h.peek();\n"
        "            if (v.n != 7) { return -1; }\n"
        "        }\n"
        "        return h.c.n;\n")));
}

// A view binds with plain `=`. §4.6 demands `#=` for a `#T` result and must NOT
// demand it here — that is the entire readability claim: the spelling at the
// call site tells the two apart.
TEST(BorrowReturn, PlainBindOfAViewIsAccepted) {
    EXPECT_EQ("", errorOf(driver(
        "        Holder h #= heap Holder(7);\n"
        "        Cell v = h.peek();\n"
        "        return v.n;\n")));
}

// §4.7's headline, and the `keyAt` bug this spec opened with: claiming a title
// over a view is caught at the line that makes the mistake, with no reference
// to the callee's body.
TEST(BorrowReturn, SharpOnAViewResultIsRejected) {
    EXPECT_EQ("CAJETA_ERROR_TRANSFER_OF_VIEW_RESULT", errorOf(driver(
        "        Holder h #= heap Holder(7);\n"
        "        Cell v #= h.peek();\n"
        "        return v.n;\n")));
}

// The gap closer. A `#T` method that forwards a view result is the exact shape
// §4.5's provenance walk misses (a MethodCallExpression, not an identifier).
// With `^` on the callee it is decidable from the signature alone.
TEST(BorrowReturn, SharpReturnOfViewResultIsRejected) {
    EXPECT_EQ("CAJETA_ERROR_OWNED_RETURN_OF_BORROW", errorOf(
        std::string(kHolder) +
        "public final class F {\n"
        "    public static #Cell forge(Holder h) { return h.peek(); }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Holder h #= heap Holder(7);\n"
        "        Cell v #= F.forge(h);\n"
        "        return v.n;\n"
        "    }\n"
        "}\n"));
}

// ------------------------------------------------------- the body restriction

// Permitted: interior reads (kHolder.peek itself), `this`, and other `^T`
// results. Delegation through a second view must stay legal or the stance
// cannot describe a wrapper.
TEST(BorrowReturn, ViewMayReturnThisAndDelegateToAnotherView) {
    EXPECT_EQ(7, runI32(
        std::string(kHolder) +
        "public final class Wrap {\n"
        "    public Holder h;\n"
        "    public Wrap(int32 v) { this.h #= heap Holder(v); }\n"
        "    public ^Wrap self() { return this; }\n"
        // NB: not `view()` — VIEW is a lexer keyword (CajetaLexer.g4:103), so
        // it cannot name a method. Pre-existing and unrelated to `^`, but it
        // costs a confusing syntax error if you reach for the obvious name.
        "    public ^Cell borrowed() { return this.h.peek(); }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Wrap w #= heap Wrap(7);\n"
        "        {\n"
        "            Cell v = w.self().borrowed();\n"
        "            if (v.n != 7) { return -1; }\n"
        "        }\n"
        "        return w.h.c.n;\n"
        "    }\n"
        "}\n"));
}

// Forbidden: a fresh allocation. Nothing else will ever free it, so a view
// return is a guaranteed leak rather than a lifetime question.
TEST(BorrowReturn, ViewOfFreshAllocationIsRejected) {
    EXPECT_EQ("CAJETA_ERROR_VIEW_RETURN_NOT_INTERIOR", errorOf(
        std::string(kHolder) +
        "public final class F {\n"
        "    public static ^Cell make() { return heap Cell(1); }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() { Cell v = F.make(); return v.n; }\n"
        "}\n"));
}

// Forbidden: an owned local — same leak, one indirection later.
TEST(BorrowReturn, ViewOfOwnedLocalIsRejected) {
    EXPECT_EQ("CAJETA_ERROR_VIEW_RETURN_NOT_INTERIOR", errorOf(
        std::string(kHolder) +
        "public final class F {\n"
        "    public static ^Cell make() {\n"
        "        Cell x #= heap Cell(1);\n"
        "        return x;\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() { Cell v = F.make(); return v.n; }\n"
        "}\n"));
}

// Forbidden: a `#T` result. The title arrives and is then disclaimed, so the
// value dies with nobody holding it.
TEST(BorrowReturn, ViewOfOwnedResultIsRejected) {
    EXPECT_EQ("CAJETA_ERROR_VIEW_RETURN_NOT_INTERIOR", errorOf(
        std::string(kHolder) +
        "public final class F {\n"
        "    public static #Cell own() { return heap Cell(1); }\n"
        "    public static ^Cell borrowed() { return F.own(); }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() { Cell v = F.borrowed(); return v.n; }\n"
        "}\n"));
}

// Forbidden: a parameter. `^T` promises the result is interior to the RECEIVER,
// which is what makes it decidable; a parameter's lifetime is the caller's and
// is exactly the ambiguity CAJETA_ERROR_BORROW_RETURN_MULTI_PARAM already
// refuses to guess at.
TEST(BorrowReturn, ViewOfParameterIsRejected) {
    EXPECT_EQ("CAJETA_ERROR_VIEW_RETURN_NOT_INTERIOR", errorOf(
        std::string(kHolder) +
        "public final class F {\n"
        "    public static ^Cell pass(Cell c) { return c; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Holder h #= heap Holder(7);\n"
        "        Cell v = F.pass(h.peek());\n"
        "        return v.n;\n"
        "    }\n"
        "}\n"));
}

// Forbidden at the DECLARATION: `^` over a value-semantics return. The caller
// receives a copy either way, so there is nothing to view — the sigil could
// only mislead. Caught in Method::generatePrototype beside the other
// signature-shape checks, before any body is examined.
TEST(BorrowReturn, ViewOfPrimitiveReturnIsRejectedAtTheDeclaration) {
    EXPECT_EQ("CAJETA_ERROR_VIEW_RETURN_OF_VALUE", errorOf(
        "package test;\n"
        "public final class F {\n"
        "    public static ^int32 bad() { return 5; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() { return F.bad(); }\n"
        "}\n"));
}

// ------------------------------------------------------------------ the sigil

// `^` stays infix xor. The prefix reading is return-type position only, so no
// existing expression changes meaning (plan 8.2.1's sigil decision).
TEST(BorrowReturn, CaretRemainsInfixXor) {
    EXPECT_EQ(6, runI32(
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32 a = 5;\n"
        "        int32 b = 3;\n"
        "        return a ^ b;\n"
        "    }\n"
        "}\n"));
}
