//
// L3 lambda tests — capture-time `#name` transfer + lifetime.
//
// L3-1 (this file's initial slice): syntax + use-after-move marking.
// `#name` inside a lambda body marks the named outer binding as a
// transfer capture — ownership conceptually moves into the closure and
// subsequent reads of the outer name surface as use-after-move at
// compile time. Drop-chain transfer (the closure becoming the actual
// owner that frees at its own drop) is L3-3; for now the outer's drop
// still fires at scope exit, which is safe as long as the closure
// doesn't escape its declaring scope (L3-2's enforcement).
//
// Out of scope (later L3 sub-slices):
//   - L3-2: lifetime/escape check — closure returning past its borrows
//   - L3-3: closure drop-chain ownership (free at closure drop, not at
//           the original binding's scope exit)
//   - `#this` transfer (Rule 4) — needs `this` capture support first
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

// `#arr` inside the lambda body transfers ownership of the heap value
// into the closure. The body itself uses the captured pointer to call a
// method; within scope, before the original binding's drop fires, the
// pointer is still valid.
TEST(LambdaL3Tests, transferCaptureRunsWithinScope) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] arr = new int32[5];\n"
        "        () -> int64 fn = () -> #arr.count();\n"
        "        return (int32) fn();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 5);
}

// Outer use of the transferred name AFTER the lambda is created surfaces
// as use-after-move at compile time. This is what makes `#` meaningful:
// the static check catches a use that would have aliased the moved-out
// owner.
TEST(LambdaL3Tests, outerUseAfterTransferIsCompileError) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] arr = new int32[3];\n"
        "        () -> int64 fn = () -> #arr.count();\n"
        "        int64 size = arr.count();\n"  // use-after-move
        "        return (int32) size;\n"
        "    }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.D");
        FAIL() << "expected use-after-move on outer read after transfer";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_USE_AFTER_MOVE");
        EXPECT_NE(e.getMessage().find("arr"), std::string::npos);
    }
}

// `#name` only triggers transfer mode for heap values. On a primitive
// (Rule 1 says primitives capture by value, no exceptions), the `#`
// marker is a no-op for capture categorization — the primitive copies
// into the captures struct as usual, and the outer binding stays
// readable. Documents the deliberate Rule-1-wins behaviour.
TEST(LambdaL3Tests, transferOnPrimitiveIsNoOp) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32 base = 10;\n"
        "        (int32) -> int32 fn = x -> x + #base;\n"
        "        int32 stillReadable = base;\n"  // not moved
        "        return fn(5) + stillReadable;\n"  // 15 + 10 = 25
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 25);
}

// Mixed: one transfer + one borrow in the same lambda. The transfer
// name is marked moved; the borrow name stays readable.
TEST(LambdaL3Tests, transferAndBorrowCoexist) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] taken = new int32[2];\n"
        "        int32[] kept = new int32[7];\n"
        "        () -> int64 fn = () -> #taken.count() + kept.count();\n"
        "        int64 keptSize = kept.count();\n"  // still readable
        "        return (int32) fn() + (int32) keptSize;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 2 + 7 + 7);
}

// ---------------------------------------------------------------------
// L3-2: lifetime / escape check
// ---------------------------------------------------------------------

// Helper for the escape-check tests below: a method that builds a
// closure capturing a local heap value by borrow and returns it. With
// the L3-2 check in place this must throw at compile time — the
// borrowed local would be dead by the time the caller invoked the
// returned closure.
namespace {

void expectBorrowEscape(const std::string& src) {
    try {
        CajetaJit::compile(src, "test.D");
        FAIL() << "expected borrow-escape error for returned closure";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_BORROW_ESCAPE");
        EXPECT_NE(e.getMessage().find("borrow"), std::string::npos);
    }
}

} // namespace

// Returning a function-typed local that holds a borrow capture is a
// compile error — the most common shape of the dangling-closure bug.
TEST(LambdaL3Tests, returnClosureWithBorrowCaptureIsError) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static () -> int64 mkFn() {\n"
        "        int32[] arr = new int32[3];\n"
        "        () -> int64 fn = () -> arr.count();\n"
        "        return fn;\n"
        "    }\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    expectBorrowEscape(src);
}

// Inline-construction shape: `return () -> ...;` without naming the
// closure. The check fires after the lambda's generateCode populates
// its borrow flag.
TEST(LambdaL3Tests, returnFreshLambdaWithBorrowCaptureIsError) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static () -> int64 mkFn() {\n"
        "        int32[] arr = new int32[3];\n"
        "        return () -> arr.count();\n"
        "    }\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    expectBorrowEscape(src);
}

// The "allowed-to-escape" cases below only verify that the L3-2 check
// doesn't over-fire — i.e. compilation completes without throwing
// CAJETA_ERROR_BORROW_ESCAPE. They don't call the returned closure: the
// closure record itself is still stack-allocated (L2-1 design) so
// invoking it after the producing method returns would dereference a
// dead frame. Heap-allocation of escaping closures is L3-3's concern;
// once that lands, these tests can grow assertions.

// Closures whose captures are all by-value (primitives) — the closure
// holds copies, not borrows, so the borrow check has nothing to flag.
TEST(LambdaL3Tests, returnClosureWithOnlyValueCapturesAllowed) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static (int32) -> int32 mkAdder() {\n"
        "        int32 base = 10;\n"
        "        (int32) -> int32 fn = x -> x + base;\n"
        "        return fn;\n"
        "    }\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    SUCCEED();
}

// Closures that transfer ownership of their heap captures via `#name`
// — again no borrows, so the borrow check passes.
TEST(LambdaL3Tests, returnClosureWithOnlyTransferCapturesAllowed) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static () -> int64 mkFn() {\n"
        "        int32[] arr = new int32[4];\n"
        "        () -> int64 fn = () -> #arr.count();\n"
        "        return fn;\n"
        "    }\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    SUCCEED();
}

// Non-capturing closures (no outer references at all). Ensures the
// check doesn't over-fire on the empty-capture case.
TEST(LambdaL3Tests, returnNonCapturingClosureAllowed) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static () -> int32 mkConst() {\n"
        "        () -> int32 fn = () -> 42;\n"
        "        return fn;\n"
        "    }\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    SUCCEED();
}

// ---------------------------------------------------------------------
// L3-3: closure drop chain — escapable closures + drop accounting
// ---------------------------------------------------------------------

namespace {

// Two-step helper mirroring DropChainTests.observe: `run()` does the
// work (its return fires the drop chain), `read()` reads the post-drop
// count. Closure-related drop semantics aren't visible from within the
// producing method since drops happen at its return, so the split is
// necessary.
int64_t observeDrops(const std::string& body) {
    std::string src =
        std::string("package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Cajeta.dropCountReset();\n"
        "        ") + body + "\n"
        "        return 0;\n"
        "    }\n"
        "    public static int64 read() {\n"
        "        return Cajeta.dropCount();\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto runFn = jit->lookup<int32_t (*)()>("run");
    runFn();
    auto readFn = jit->lookup<int64_t (*)()>("read");
    return readFn();
}

} // namespace

// Transfer capture: outer's drop entry is deactivated (no double-free),
// the closure's drop entry fires once and the synthesized drop function
// frees the transferred heap object + the captures struct + the closure
// record. The drop chain registers one increment for the active entry.
TEST(LambdaL3Tests, transferCaptureClosureDropsOnce) {
    EXPECT_EQ(observeDrops(
        "int32[] arr = new int32[3];\n"
        "        () -> int64 fn = () -> #arr.count();"), 1);
}

// Borrow capture: outer's array keeps its active drop entry (closure
// merely borrowed the pointer). Two increments — one for the closure
// (which only frees its own heap-allocated record + captures struct,
// not the borrowed array) and one for the outer's array.
TEST(LambdaL3Tests, borrowCaptureClosureDropsBothEntries) {
    EXPECT_EQ(observeDrops(
        "int32[] arr = new int32[3];\n"
        "        () -> int64 fn = () -> arr.count();"), 2);
}

// Value capture: outer primitive has no drop entry. Closure's drop
// entry fires once.
TEST(LambdaL3Tests, valueCaptureClosureDropsOnce) {
    EXPECT_EQ(observeDrops(
        "int32 base = 10;\n"
        "        (int32) -> int32 fn = x -> x + base;"), 1);
}

// Non-capturing closure: the record is a private global constant, not
// heap-allocated, but the function-typed local still gets a drop entry
// (we don't statically know if the value came from a non-capturing
// literal or a possibly-capturing call). At scope exit
// __cajeta_closure_drop reads drop_fn=null on the global and no-ops —
// but the entry's fire still increments the count.
TEST(LambdaL3Tests, nonCapturingClosureLocalIncrementsCount) {
    EXPECT_EQ(observeDrops(
        "() -> int32 fn = () -> 42;"), 1);
}

// End-to-end escape: return a closure that transferred a heap value,
// then invoke it from the caller. Pre-L3-3 the heap was freed at the
// producer's return and the call would segfault. Now the closure owns
// the heap, the producer's drop entry was deactivated on return, and
// the caller's local takes ownership.
TEST(LambdaL3Tests, returnedTransferCaptureClosureCallable) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static () -> int64 mkFn() {\n"
        "        int32[] arr = new int32[5];\n"
        "        () -> int64 fn = () -> #arr.count();\n"
        "        return fn;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        () -> int64 fn = mkFn();\n"
        "        int64 size = fn();\n"
        "        return (int32) size;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 5);
}

// End-to-end escape: value-capture closure. Primitives copy into the
// captures struct at the producing site; heap-alloc'd struct persists
// past mkAdder's return because the closure now owns it.
TEST(LambdaL3Tests, returnedValueCaptureClosureCallable) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static (int32) -> int32 mkAdder() {\n"
        "        int32 base = 10;\n"
        "        (int32) -> int32 fn = x -> x + base;\n"
        "        return fn;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        (int32) -> int32 fn = mkAdder();\n"
        "        return fn(5);\n"  // 5 + 10
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 15);
}

// End-to-end escape: non-capturing closure. Record lives in a global,
// so the producer's stack frame death is irrelevant.
TEST(LambdaL3Tests, returnedNonCapturingClosureCallable) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static () -> int32 mkConst() {\n"
        "        () -> int32 fn = () -> 42;\n"
        "        return fn;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        () -> int32 fn = mkConst();\n"
        "        return fn();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// Drop count after escape + invocation: the caller's local owns the
// returned closure and fires its drop entry at scope exit. mkFn's drop
// entries are deactivated (transferred). Caller's run() fires one drop
// for fn (which itself frees the transferred arr + captures + closure).
TEST(LambdaL3Tests, escapedClosureDropFiresInCaller) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static () -> int64 mkFn() {\n"
        "        int32[] arr = new int32[4];\n"
        "        () -> int64 fn = () -> #arr.count();\n"
        "        return fn;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Cajeta.dropCountReset();\n"
        "        () -> int64 fn = mkFn();\n"
        "        int64 unused = fn();\n"
        "        return 0;\n"
        "    }\n"
        "    public static int64 read() {\n"
        "        return Cajeta.dropCount();\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    jit->lookup<int32_t (*)()>("run")();
    EXPECT_EQ(jit->lookup<int64_t (*)()>("read")(), 1);
}

// ---------------------------------------------------------------------
// Transfer (`#name`) inside nested blocks of a lambda body
// ---------------------------------------------------------------------
//
// Before the lambda-capture walker fix, collectTransferNames descended
// through `getChildren()` for any Statement subtype without a dedicated
// handler. LabelStatement (the if-branch wrapper), WhileStatement,
// ForStatement, DoStatement, ScopeStatement, EnhancedForStatement all
// hold their inner block as a private member that isn't in `children`,
// so a `#name` reference inside one of those nested bodies was never
// registered as a transfer. The downstream effects were silent and
// nasty: the outer scope's drop still fired, the closure thought it
// only borrowed, and outer reads after the lambda escaped the
// use-after-move check.
//
// These tests pin transfer-name registration at the if-branch and
// for-body sites — if the walker stops descending into either, the
// outer use-after-move check fails to fire and these tests will
// regress to the no-error path.

// Transfer-name registered when `#arr` appears inside the if-then
// block of a lambda body. Outer use after the lambda is created must
// still trip use-after-move.
TEST(LambdaL3Tests, transferInsideIfBranchOfLambdaMovesOuter) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] arr = new int32[3];\n"
        "        (boolean) -> int64 fn = (b) -> {\n"
        "            if (b) {\n"
        "                return #arr.count();\n"
        "            }\n"
        "            return 0L;\n"
        "        };\n"
        "        int64 size = arr.count();\n"
        "        return (int32) (fn(true) + size);\n"
        "    }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.D");
        FAIL() << "expected use-after-move on outer read after transfer in if-branch";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_USE_AFTER_MOVE");
        EXPECT_NE(e.getMessage().find("arr"), std::string::npos);
    }
}

// Same shape but the `#name` lives inside a `for`-loop body.
TEST(LambdaL3Tests, transferInsideForBodyOfLambdaMovesOuter) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] arr = new int32[3];\n"
        "        (int32) -> int64 fn = (n) -> {\n"
        "            int64 acc = 0L;\n"
        "            for (int32 i = 0; i < n; i = i + 1) {\n"
        "                acc = acc + #arr.count();\n"
        "            }\n"
        "            return acc;\n"
        "        };\n"
        "        int64 size = arr.count();\n"
        "        return (int32) (fn(1) + size);\n"
        "    }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.D");
        FAIL() << "expected use-after-move on outer read after transfer in for body";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_USE_AFTER_MOVE");
        EXPECT_NE(e.getMessage().find("arr"), std::string::npos);
    }
}

// Positive control: the transfer-only-in-nested-block lambda compiles
// and runs (no outer use after) — confirms the if-branch transfer
// works end-to-end, not just that the static check fires.
TEST(LambdaL3Tests, transferInsideIfBranchOfLambdaRunsWhenNoOuterUse) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] arr = new int32[6];\n"
        "        (boolean) -> int64 fn = (b) -> {\n"
        "            if (b) {\n"
        "                return #arr.count();\n"
        "            }\n"
        "            return 0L;\n"
        "        };\n"
        "        return (int32) fn(true);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 6);
}
