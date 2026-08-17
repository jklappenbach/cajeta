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
        "    public ^Cell make() { return heap Cell(1); }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        F f #= heap F();\n"
        "        Cell v = f.make();\n"
        "        return v.n;\n"
        "    }\n"
        "}\n"));
}

// Forbidden: an owned local — same leak, one indirection later.
TEST(BorrowReturn, ViewOfOwnedLocalIsRejected) {
    EXPECT_EQ("CAJETA_ERROR_VIEW_RETURN_NOT_INTERIOR", errorOf(
        std::string(kHolder) +
        "public final class F {\n"
        "    public ^Cell make() {\n"
        "        Cell x #= heap Cell(1);\n"
        "        return x;\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        F f #= heap F();\n"
        "        Cell v = f.make();\n"
        "        return v.n;\n"
        "    }\n"
        "}\n"));
}

// Forbidden: a `#T` result. The title arrives and is then disclaimed, so the
// value dies with nobody holding it.
TEST(BorrowReturn, ViewOfOwnedResultIsRejected) {
    EXPECT_EQ("CAJETA_ERROR_VIEW_RETURN_NOT_INTERIOR", errorOf(
        std::string(kHolder) +
        "public final class F {\n"
        "    public static #Cell own() { return heap Cell(1); }\n"
        "    public ^Cell borrowed() { return F.own(); }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        F f #= heap F();\n"
        "        Cell v = f.borrowed();\n"
        "        return v.n;\n"
        "    }\n"
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
        "    public ^Cell pass(Cell c) { return c; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Holder h #= heap Holder(7);\n"
        "        F f #= heap F();\n"
        "        Cell v = f.pass(h.peek());\n"
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

// ---------------------------------------------------- review-hardening pins
//
// Each test below pins a hole the adversarial review of the first cut found
// and this file's fixtures did not cover. The recurring root cause was a
// divergent re-implementation of `Method::exprIsInteriorRead`; the checks now
// use it directly, and these tests are the proof of the difference.

// The first cut permitted ANY DotExpression, so a view into a frame-owned
// local's interior compiled — a guaranteed UAF (the local's drop frees the
// storage the view points into at scope exit).
TEST(BorrowReturn, ViewOfALocalObjectsFieldIsRejected) {
    EXPECT_EQ("CAJETA_ERROR_VIEW_RETURN_NOT_INTERIOR", errorOf(
        std::string(kHolder) +
        "public final class W {\n"
        "    public ^Cell f() {\n"
        "        Holder h #= heap Holder(1);\n"
        "        return h.c;\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        W w #= heap W();\n"
        "        Cell v = w.f();\n"
        "        return v.n;\n"
        "    }\n"
        "}\n"));
}

// The first cut's delegation arm accepted any resolved `^` callee without
// looking at the RECEIVER, so a dying local laundered a view through a nested
// `^` call. Every hop of the chain must ride `this`'s lifetime.
TEST(BorrowReturn, ViewDelegationOnALocalReceiverIsRejected) {
    EXPECT_EQ("CAJETA_ERROR_VIEW_RETURN_NOT_INTERIOR", errorOf(
        std::string(kHolder) +
        "public final class W {\n"
        "    public ^Cell grab() {\n"
        "        Holder tmp #= heap Holder(9);\n"
        "        return tmp.peek();\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        W w #= heap W();\n"
        "        Cell v = w.grab();\n"
        "        return v.n;\n"
        "    }\n"
        "}\n"));
}

// The one-hop launder: a `^` result parked in a local and returned under a
// `#` declaration. Closed by recording call-borrow provenance from the
// SIGNATURE (isReturnsView) instead of only the body scan, so §4.5's
// existing check finds the origin.
TEST(BorrowReturn, SharpReturnOfViewResultThroughALocalIsRejected) {
    EXPECT_EQ("CAJETA_ERROR_OWNED_RETURN_OF_BORROW", errorOf(
        std::string(kHolder) +
        "public final class W {\n"
        "    public Holder h;\n"
        "    public W(int32 v) { this.h #= heap Holder(v); }\n"
        "    public ^Cell borrowed() { return this.h.peek(); }\n"
        "}\n"
        "public final class F {\n"
        "    public static #Cell forge(W w) {\n"
        "        Cell v = w.borrowed();\n"
        "        return v;\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        W w #= heap W(7);\n"
        "        Cell v #= F.forge(w);\n"
        "        return v.n;\n"
        "    }\n"
        "}\n"));
}

// `return stack Cell(1)` in a `^` body: before returnsStackValue() learned
// about `^`, the method silently became an sret VALUE method whose returns
// exited codegen before the body check could see them.
TEST(BorrowReturn, ViewOfStackConstructionIsRejected) {
    EXPECT_EQ("CAJETA_ERROR_VIEW_RETURN_NOT_INTERIOR", errorOf(
        std::string(kHolder) +
        "public final class F {\n"
        "    public ^Cell make() { return stack Cell(1); }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        F f #= heap F();\n"
        "        Cell v = f.make();\n"
        "        return v.n;\n"
        "    }\n"
        "}\n"));
}

// An identity reference cast is a stance-laundering spell unless peeled —
// the same 6.2.6c peel the disarm/escape checks already do. Both the
// callee-side forward and the caller-side receipt are pinned.
TEST(BorrowReturn, CastCannotLaunderAViewPastTheSharpReturnCheck) {
    EXPECT_EQ("CAJETA_ERROR_OWNED_RETURN_OF_BORROW", errorOf(
        std::string(kHolder) +
        "public final class F {\n"
        "    public static #Cell f(Holder h) { return (Cell) h.peek(); }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Holder h #= heap Holder(7);\n"
        "        Cell v #= F.f(h);\n"
        "        return v.n;\n"
        "    }\n"
        "}\n"));
}

TEST(BorrowReturn, CastCannotLaunderAViewPastTheReceiptCheck) {
    EXPECT_EQ("CAJETA_ERROR_TRANSFER_OF_VIEW_RESULT", errorOf(driver(
        "        Holder h #= heap Holder(7);\n"
        "        Cell v #= (Cell) h.peek();\n"
        "        return v.n;\n")));
}

// `^` on `operator#[]` is a contradiction in one signature: that operator
// exists to EXTRACT a title, and its dispatch path never consults the stance.
TEST(BorrowReturn, ViewOnTheExtractingIndexOperatorIsRejected) {
    EXPECT_EQ("CAJETA_ERROR_VIEW_ON_EXTRACTING_OPERATOR", errorOf(
        std::string(kHolder) +
        "public final class Bag {\n"
        "    public Cell x;\n"
        "    public Bag() { this.x #= heap Cell(3); }\n"
        "    public ^Cell operator#[](int32 i) { return this.x; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() { return 0; }\n"
        "}\n"));
}

// A method reference to a `^` method would erase the stance at the seam —
// and a `#R` function type would even license a `#=` receipt over the
// receiver's interior. Rejected until `^` has a reference-stance story.
TEST(BorrowReturn, MethodReferenceToAViewMethodIsRejected) {
    EXPECT_EQ("CAJETA_ERROR_VIEW_REFERENCE_UNSUPPORTED", errorOf(driver(
        "        Holder h #= heap Holder(7);\n"
        "        (Holder) -> Cell f = Holder::peek;\n"
        "        Cell v = f(h);\n"
        "        return v.n;\n")));
}

// `(P) -> ^R` has no ABI form — function values carry an ownership stance
// only, and silently classifying the CARET picked the sret VALUE ABI.
TEST(BorrowReturn, ViewInFunctionTypePositionIsRejected) {
    EXPECT_EQ("CAJETA_ERROR_VIEW_REFERENCE_UNSUPPORTED", errorOf(driver(
        "        Holder h #= heap Holder(7);\n"
        "        (Holder) -> ^Cell f = Holder::peek;\n"
        "        return 0;\n")));
}

// Interface-typed call sites apply the INTERFACE's stance rules, so a `#`
// implementor under a `^` declaration is a leak (and the converse a double
// free). The stance is part of the implementation obligation.
TEST(BorrowReturn, ImplementorStanceMustMatchTheInterfaces) {
    EXPECT_EQ("CAJETA_ERROR_VIEW_STANCE_MISMATCH", errorOf(
        std::string(kHolder) +
        "public interface Peeker {\n"
        "    ^Cell look();\n"
        "}\n"
        "public final class Own implements Peeker {\n"
        "    public #Cell look() { return heap Cell(1); }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Own o #= heap Own();\n"
        "        return 0;\n"
        "    }\n"
        "}\n"));
}

// `this.cells[i]` — an index into interior storage is still interior
// (`Method::exprIsInteriorRead`'s ArrayIndex recursion). The first cut
// rejected the spec's own motivating indexer shape.
TEST(BorrowReturn, IndexedInteriorReadIsPermitted) {
    EXPECT_EQ(7, runI32(
        std::string(kHolder) +
        "public final class W {\n"
        "    public Cell[] cells;\n"
        "    public W(int32 v) {\n"
        "        this.cells #= heap Cell[2];\n"
        "        this.cells[0] #= heap Cell(v);\n"
        "    }\n"
        "    public ^Cell at(int32 i) { return this.cells[i]; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        W w #= heap W(7);\n"
        "        Cell v = w.at(0);\n"
        "        return v.n;\n"
        "    }\n"
        "}\n"));
}

// `return null` — the natural miss for a view-returning lookup. null is a
// TextLiteralExpression, which the first cut's identifier arm could never
// match (dead code the review caught).
TEST(BorrowReturn, ViewMayReturnNull) {
    EXPECT_EQ(7, runI32(
        std::string(kHolder) +
        "public final class W {\n"
        "    public Holder h;\n"
        "    public W(int32 v) { this.h #= heap Holder(v); }\n"
        "    public ^Cell find(boolean hit) {\n"
        "        if (hit) { return this.h.peek(); }\n"
        "        return null;\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        W w #= heap W(7);\n"
        "        Cell v = w.find(true);\n"
        "        return v.n;\n"
        "    }\n"
        "}\n"));
}

// A STATIC method has no receiver, so there is no lifetime for a view to
// ride — rejected at the declaration (which also keeps the multi-parameter
// borrow check's `use #` fix-suggestion out of `^`'s way).
TEST(BorrowReturn, ViewOnAStaticMethodIsRejected) {
    EXPECT_EQ("CAJETA_ERROR_VIEW_RETURN_STATIC", errorOf(
        std::string(kHolder) +
        "public final class F {\n"
        "    public static ^Cell s(Holder h) { return null; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() { return 0; }\n"
        "}\n"));
}

// The value-semantics rejection reaches ABSTRACT declarations too — the
// first cut sat below the abstract early-return, so an interface could
// declare `^int32` and every caller carried a view stance over a copy.
TEST(BorrowReturn, ViewOfPrimitiveOnAnInterfaceIsRejected) {
    EXPECT_EQ("CAJETA_ERROR_VIEW_RETURN_OF_VALUE", errorOf(
        "package test;\n"
        "public interface P2 {\n"
        "    ^int32 x();\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() { return 0; }\n"
        "}\n"));
}

// …but a template's `^V` instantiated at a primitive DEMOTES to a plain
// copy instead of erroring (exactly as `#` on a primitive is a no-op), or
// the spec's motivating keyAt container could not hold primitives. At a
// class type the view stance survives and the interior stays alive.
TEST(BorrowReturn, TemplateViewDemotesAtPrimitiveAndViewsAtClass) {
    EXPECT_EQ(7, runI32(
        std::string(kHolder) +
        "public class Box<V> {\n"
        "    public V v;\n"
        "    public Box(#V x) { this.v #= x; }\n"
        "    public ^V get() { return this.v; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Box<int32> b #= heap Box<int32>(5);\n"
        "        int32 k = b.get();\n"
        "        Box<Cell> bc #= heap Box<Cell>(#(heap Cell(7)));\n"
        "        Cell cv = bc.get();\n"
        "        if (k != 5) { return -1; }\n"
        "        return cv.n;\n"
        "    }\n"
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
