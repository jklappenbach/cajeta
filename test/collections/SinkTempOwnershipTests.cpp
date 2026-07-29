//
// title-tracking 6.2.5 — sink/temp ownership. Spec §4.1.1: an owned rvalue
// (fresh construction, flag-true call result) in ARGUMENT position
// SURRENDERS the title (flag true) — the callee's formal entry owns the
// temp exactly as `#x` would. Before this item, plain creator temps and
// flagged-call-result temps contributed no transfer-word bit, so nobody
// ever freed them (systemic pre-existing leak, recorded in 6.2.3 notes).
//
// Receivers have no flag bit (`this` is borrowed, no #this), so a fresh
// temp receiver is reclaimed CALLER-side after the call — but only when
// the outer return provably cannot borrow the receiver's interior: void/
// primitive returns, or a class-pointer return that came back flag-TRUE
// (fresh/owned) and is not the receiver itself (builder `return this`).
// A flag-0 class return may be a borrow of the receiver's innards, so the
// receiver is left alone (today's leak, documented by the last pin here).
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

// All imports MUST precede every class declaration (mid-file imports
// crash the parser — 6.2.3 harness trap).
const char* kCellSrc =
    "package test;\n"
    "import cajeta.collection.ArrayList;\n"
    "@AutoHash\n"
    "public class Cell {\n"
    "    public int32 n;\n"
    "    public Cell(int32 nn) { this.n = nn; }\n"
    "    public int32 doubled() { return this.n * 2; }\n"
    "    public #Cell plusOne() { return heap Cell(this.n + 1); }\n"
    "    public static boolean operator== (Cell a, Cell b) { return a.n == b.n; }\n"
    "}\n";

// Same-shaped churn: clobbers a freed Cell's storage so a dangling read
// returns garbage instead of stale-but-intact bytes.
const char* kChurn =
    "        int32 c = 0;\n"
    "        int32 churn = 0;\n"
    "        while (c < 64) { Cell j = heap Cell(1000 + c); churn = churn + j.n; c = c + 1; }\n"
    "        if (churn < 0) { return churn; }\n";

int32_t runI32(const std::string& src, const char* entryClass = "test.D") {
    auto jit = CajetaJit::compile(src, entryClass);
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

}  // namespace

// ===================== Argument position =====================

// `probe(heap Cell(7))` — the fresh creator surrenders (spec §4.1.1): the
// callee's formal entry arrives flag-true, nothing consumes it, so the
// CALLEE's exit drop reclaims the temp. No leak, value intact.
TEST(SinkTempOwnershipTests, creatorTempArgIsDroppedByCallee) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static int32 probe(Cell c) { return c.n; }\n"
        "    public static int32 work() {\n"
        "        int32 t = probe(heap Cell(7));\n" +
        std::string(kChurn) +
        "        return t;\n"
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

// `xs.add(heap Cell(5))` — the surrendered temp's word bit reaches the
// entry-bit store: the LIST owns the element (readable afterwards, no
// dangle), and the list's teardown reclaims it.
TEST(SinkTempOwnershipTests, creatorTempArgIntoContainerIsOwned) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static int32 work() {\n"
        "        int32 t = 0;\n"
        "        {\n"
        "            ArrayList<Cell> xs = heap ArrayList<Cell>();\n"
        "            xs.add(heap Cell(5));\n" +
        std::string(kChurn) +
        "            t = xs.get(0).n;\n"
        "        }\n"
        "        return t;\n"
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

// `sink(make())` — a flag-true call result in argument position forwards
// its flag into the outer call's word (spec §4.4.2): the sink's formal
// owns the temp and the callee exit drop reclaims it.
TEST(SinkTempOwnershipTests, flaggedResultTempArgForwardsTitle) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static #Cell make() { return heap Cell(11); }\n"
        "    public static int32 sink(Cell c) { return c.n; }\n"
        "    public static int32 work() {\n"
        "        int32 t = sink(make());\n" +
        std::string(kChurn) +
        "        return t;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int32 t = work();\n"
        "        int64 leaked = Cajeta.liveCount() - base;\n"
        "        return (int32) (leaked * 100) + t;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 11);
}

// `heap Wrapper(heap Cell(5))` — the plain creator CTOR argument
// surrenders the same way: the ctor's consuming store (`this.c #= c`)
// sees bit 1, the wrapper owns the cell, teardown reclaims both.
TEST(SinkTempOwnershipTests, creatorTempCtorArgSurrenders) {
    std::string src = std::string(kCellSrc) +
        "public class Wrapper {\n"
        "    public Cell c;\n"
        "    public Wrapper(Cell cc) { this.c #= cc; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 work() {\n"
        "        int32 t = 0;\n"
        "        {\n"
        "            Wrapper w = heap Wrapper(heap Cell(5));\n" +
        std::string(kChurn) +
        "            t = w.c.n;\n"
        "        }\n"
        "        return t;\n"
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

// ===================== Receiver position =====================

// `(heap Cell(7)).doubled()` — primitive return: the fresh temp receiver
// is reclaimed caller-side right after the call.
TEST(SinkTempOwnershipTests, creatorTempReceiverReclaimedOnPrimitiveReturn) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static int32 work() {\n"
        "        int32 t = (heap Cell(7)).doubled();\n" +
        std::string(kChurn) +
        "        return t;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int32 t = work();\n"
        "        int64 leaked = Cajeta.liveCount() - base;\n"
        "        return (int32) (leaked * 100) + t;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 14);
}

// `(heap Cell(1)).plusOne().plusOne().doubled()` — a temp receiver is
// reclaimed ONLY on a void/primitive-returning call: even a flag-true
// fresh class result may be a WRAPPER borrowing the receiver as its inner
// source (FluentStream chains SIGSEGV'd when intermediates were reclaimed
// mid-chain), so the two class-returning links keep their receivers alive
// (bounded leak, 2 objects) and only doubled()'s primitive return
// reclaims its receiver. t = 6, leaked = 2.
TEST(SinkTempOwnershipTests, chainReclaimsOnlyPrimitiveReturnLink) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static int32 work() {\n"
        "        int32 t = (heap Cell(1)).plusOne().plusOne().doubled();\n" +
        std::string(kChurn) +
        "        return t;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int32 t = work();\n"
        "        int64 leaked = Cajeta.liveCount() - base;\n"
        "        return (int32) (leaked * 100) + t;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 206);
}

// SAFETY pin — a flag-0 class-pointer return may be a borrow of the
// receiver's interior, so the temp receiver is NOT dropped: `peek()`
// returns the held Cell plain; the read stays valid, and the Holder temp
// (plus the Cell it owns) intentionally leaks — 2 objects. This is the
// documented safe-leak edge, not a regression.
TEST(SinkTempOwnershipTests, borrowReturnLeavesTempReceiverAlive) {
    std::string src = std::string(kCellSrc) +
        "public class Holder {\n"
        "    public Cell c;\n"
        "    public Holder(Cell cc) { this.c #= cc; }\n"
        "    public Cell peek() { return this.c; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 work() {\n"
        "        int32 t = (heap Holder(#heap Cell(3))).peek().n;\n" +
        std::string(kChurn) +
        "        return t;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int32 t = work();\n"
        "        int64 leaked = Cajeta.liveCount() - base;\n"
        "        return (int32) (leaked * 100) + t;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 203);
}
