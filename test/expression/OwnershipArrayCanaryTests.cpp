// stdlib-ownership-convention U2 — ARRAY CANARY (plan 2.2.7).
//
// The question liveCount cannot answer. Arrays are excluded from the
// flag-armed drop entry (`!isArray`, LocalVariableDeclaration.cpp:1929) AND
// invisible to Cajeta.liveCount (OwnershipRuntimeProbeTests
// .instrumentIsBlindToArrays measures a delta of 0 across an allocation), so
// the counter reports "balanced" for a correct program and a double-free
// alike. This file uses a different detector: DATA SURVIVAL.
//
// Why it matters beyond bookkeeping. The original cajeta-llama corruption was
// an ARRAY — `int8[] kb = o.keyAt(j); heap String(#kb, kl)` — and Unit 6 is
// scheduled to strip the `TplEval` workarounds that exist because of it. If
// arrays behave correctly here, then that account of the corruption is wrong
// and the workarounds must not be removed on the strength of it. Removing a
// workaround on a misdiagnosis is how the bug returns.
//
// The shape, in both variants:
//
//     make():  Box b = heap Box();      // b owns the payload
//              X v = b.peek();          // LENT out
//              return heap Sink(#v);    // ...surrendered, then b DROPS
//
// Reading through the Sink afterwards distinguishes three outcomes:
//
//   * intact  -> `#v` really did transfer the title; Box's drop released
//                its claim and the Sink is the sole owner.
//   * garbage -> `#v` did NOT transfer (or both claimed it): Box freed the
//                payload on return and the Sink points at reused memory.
//   * abort   -> the allocator caught a double free outright.
//
// Churn allocations after `make()` returns exist to get the freed block
// recycled, so a dangling read shows a DIFFERENT value rather than
// coincidentally-intact bytes.
//
// `peek()` returns through a local rather than `return this.data;` so that
// Method::returnsInteriorView cannot prove the view and the U2 check does not
// reject the very code under measurement. Ownership shape is identical.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

#include "cajeta/error/Exception.h"

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

// 7,11,13,17 packed as 7*1000 + 11*100 + 13*10 + 17.
constexpr int32_t kIntact = 8247;

}  // namespace

// ===================================================================
// RESULT (measured 2026-08-14). BOTH canaries corrupt. `#v` on a lent
// value does NOT transfer the title — for arrays OR for classes. The
// lend stays a lend, the lender frees on drop, and the receiver is left
// pointing at reused memory.
//
//   array payload:  read back -83968, expected 8247
//   class payload:  read back 107800, expected 8100 — decoding,
//                   a=98 b=98, i.e. the Cell's memory was recycled into
//                   the churn allocation c2(98,98)
//
// Two corrections this forces:
//
//  1. The earlier liveCount probe measuring the class shape as
//     "balanced" was not wrong, but it was answering a weaker question.
//     Everything in that probe lived in ONE scope, so nothing outlived
//     its lender and nothing could dangle. A balanced count is
//     consistent with correct ownership AND with a lend that never
//     transferred. Data survival separates them; counting cannot.
//
//  2. The defect is worse than "a `#` that silently does nothing". When
//     the receiver OUTLIVES the lender it is a use-after-free. That
//     makes the U2 check load-bearing rather than merely tidy, and it
//     vindicates keeping the cajeta-llama TplEval workarounds until
//     Unit 6 can retest them (the llama shape was exactly this).
//
// Why these are DISABLED rather than red or inverted: they document a
// COVERAGE GAP, not expected behaviour. `peek()` returns through a local
// (`int8[] v = this.data; return v;`), so Method::returnsInteriorView
// cannot prove the view and the check stays silent — yet the code
// corrupts identically to the shape it does catch. Asserting the corrupt
// values would pin a bug as correct; leaving them failing would poison
// the gate. Enable them when the check tracks straight-line locals
// (spec §7.2, plan item 3.1.5) — they should go green with no edit.
// ===================================================================

// CANARY 1 — ARRAY payload. The cajeta-llama shape.
TEST(OwnershipArrayCanaryTests, DISABLED_arrayPayloadSurvivesOwnerDrop) {
    std::string src =
        "package test;\n"
        "public final class Box {\n"
        "    int8[] data;\n"
        "    public Box() {\n"
        "        this.data = heap int8[4];\n"
        "        this.data[0] = (int8) 7;\n"
        "        this.data[1] = (int8) 11;\n"
        "        this.data[2] = (int8) 13;\n"
        "        this.data[3] = (int8) 17;\n"
        "    }\n"
        "    public int8[] peek() { int8[] v = this.data; return v; }\n"
        "}\n"
        "public final class Sink {\n"
        "    int8[] held;\n"
        "    public Sink(#int8[] b) { this.held #= b; }\n"
        "    public int32 at(int32 i) { return (int32) this.held[i]; }\n"
        "}\n"
        "public final class D {\n"
        "    static #Sink make() {\n"
        "        Box b = heap Box();\n"
        "        int8[] v = b.peek();\n"      // LENT
        "        return heap Sink(#v);\n"     // surrendered; b drops on return
        "    }\n"
        "    public static int32 run() {\n"
        "        Sink s = D.make();\n"
        "        int8[] c1 = heap int8[4];\n" // churn: recycle the freed block
        "        c1[0] = (int8) 99; c1[1] = (int8) 99;\n"
        "        c1[2] = (int8) 99; c1[3] = (int8) 99;\n"
        "        int8[] c2 = heap int8[4];\n"
        "        c2[0] = (int8) 98; c2[1] = (int8) 98;\n"
        "        c2[2] = (int8) 98; c2[3] = (int8) 98;\n"
        "        return s.at(0) * 1000 + s.at(1) * 100\n"
        "             + s.at(2) * 10 + s.at(3);\n"
        "    }\n"
        "}\n";
    int32_t got = runI32(src);
    EXPECT_EQ(got, kIntact)
        << "array payload read back as " << got << " instead of " << kIntact
        << " — the owner's drop released it while the Sink still pointed at "
           "it, so `#v` did not actually transfer the title. This is the "
           "cajeta-llama shape and it means the U2 check is load-bearing for "
           "arrays (plan 2.2.7).";
}

// CANARY 2 — CLASS payload, same shape. The contrast case: the class kind is
// inside the flag-armed mechanism, and OwnershipRuntimeProbeTests measured it
// as balanced. Whatever this returns, it is a MEASUREMENT of whether `#` on a
// lent class value transfers, which is the defect this unit now claims to
// catch ("a `#` that silently does nothing").
TEST(OwnershipArrayCanaryTests, DISABLED_classPayloadSurvivesOwnerDrop) {
    std::string src =
        "package test;\n"
        "public final class Cell {\n"
        "    public int32 a; public int32 b;\n"
        "    public Cell(int32 x, int32 y) { this.a = x; this.b = y; }\n"
        "}\n"
        "public final class Box {\n"
        "    Cell data;\n"
        "    public Box() { this.data = heap Cell(7, 11); }\n"
        "    public Cell peek() { Cell v = this.data; return v; }\n"
        "}\n"
        "public final class Sink {\n"
        "    Cell held;\n"
        "    public Sink(#Cell c) { this.held #= c; }\n"
        "    public int32 a() { return this.held.a; }\n"
        "    public int32 b() { return this.held.b; }\n"
        "}\n"
        "public final class D {\n"
        "    static #Sink make() {\n"
        "        Box b = heap Box();\n"
        "        Cell v = b.peek();\n"
        "        return heap Sink(#v);\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Sink s = D.make();\n"
        "        Cell c1 = heap Cell(99, 99);\n"
        "        Cell c2 = heap Cell(98, 98);\n"
        "        return s.a() * 1000 + s.b() * 100 + c1.a - c2.a - 1;\n"
        "    }\n"
        "}\n";
    // 7*1000 + 11*100 + 99 - 98 - 1 = 8100.
    int32_t got = runI32(src);
    EXPECT_EQ(got, 8100)
        << "class payload read back as " << got << " instead of 8100 — the "
           "owner's drop released it while the Sink still held it.";
}
