// stdlib-ownership-convention U2 — EXPERIMENT, not a specification.
//
// Question: when `#x` surrenders a class-typed value that was LENT out of a
// container, does the runtime already prevent the double free?
//
// Motive. The transfer-of-a-borrow check was built on "a plain (non-`#`)
// return hands back a borrow". That premise is false — a plain return
// carries a RUNTIME title flag (SignatureAbiTests.tailCallThroughPlain-
// ReturnKeepsTitle). LocalVariableDeclaration.cpp:251 then arms a call-result
// local's drop entry FROM THAT FLAG rather than from a compile-time fact, so
// a lent result should leave the entry disarmed and the container should keep
// its single drop.
//
// But that arming is GATED (LocalVariableDeclaration.cpp:1929):
//
//     initIsFlaggedCall && callResultFlag && klass && !isArray
//         && !isStructType && !isCajetaString && !klass->isInterface()
//         && !klass->isValueType() && !klass->isSharedCapableValue()
//         && klass->hasVtablePointerAtSlotZero() && !field->getDropEntry()
//
// Arrays, structs, String, interfaces and value types are EXCLUDED. So the
// prediction under test is a split:
//
//   * vtable-bearing CLASS  (the SharedPoolServer `TcpStream` shape)
//         -> inside the mechanism, already safe, check (c) adds nothing
//   * ARRAY                 (the cajeta-llama `int8[] kb = o.keyAt(j)` shape)
//         -> outside the mechanism, genuinely unsafe, check (c) earns its keep
//
// If that holds, check (c)'s correct scope is the excluded kinds, not "plain
// returns" — a much narrower claim than the one Unit 2 started from, and the
// difference between reverting the check and keeping it.
//
// Method: Cajeta.liveCount() around a scope that allocates, lends, and
// surrenders. Balanced == every object freed exactly once. POSITIVE == leak
// (the title was dropped on the floor). NEGATIVE == double free (freed twice).
//
// The accessors below deliberately return a borrow through a LOCAL rather
// than `return this.field`, so Method::returnsInteriorView cannot prove the
// view and the U2 check does not reject the very code being measured. The
// ownership shape is identical; only the provability differs.

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

}  // namespace

// PROBE 1 — vtable-bearing CLASS, the SharedPoolServer shape.
//
//   Box takes title (like Channel.receive -> `stack Optional<T>(true, #item)`)
//   peek() lends it out   (like Optional.get)
//   Holder.put(#Cell)     (like runTurn's `#TcpStream` formal)
//
// Returns leaked*100 + value. 7 == balanced.
TEST(OwnershipRuntimeProbeTests, classBorrowSurrenderedAtArgument) {
    std::string src =
        "package test;\n"
        "public final class Cell {\n"
        "    public int32 n;\n"
        "    public Cell(int32 v) { this.n = v; }\n"
        "}\n"
        "public final class Box {\n"
        "    Cell value;\n"
        "    public Box(#Cell c) { this.value #= c; }\n"
        "    public Cell peek() { Cell v = this.value; return v; }\n"
        "}\n"
        "public final class Holder {\n"
        "    Cell held;\n"
        "    public Holder() { this.held = null; }\n"
        "    public void put(#Cell v) { this.held #= v; }\n"
        "    public int32 value() { return this.held.n; }\n"
        "}\n"
        "public final class D {\n"
        "    static int32 work() {\n"
        "        Box b = heap Box(#heap Cell(7));\n"
        "        Cell c = b.peek();\n"      // LENT out of the container
        "        Holder h = heap Holder();\n"
        "        h.put(#c);\n"              // ...surrendered to a `#Cell` formal
        "        return h.value();\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int32 t = D.work();\n"
        "        int64 leaked = Cajeta.liveCount() - base;\n"
        "        return (int32) (leaked * 100) + t;\n"
        "    }\n"
        "}\n";
    int32_t got = runI32(src);
    // Not an assertion of correctness — a measurement. Recorded so the
    // number, not the argument, decides whether check (c) is needed here.
    EXPECT_EQ(got, 7) << "class shape: leaked=" << ((got - 7) / 100)
                      << " (0 balanced, >0 leak, <0 double free)";
}

// PROBE 2 — ARRAY, the cajeta-llama shape. Same ownership story, but an
// array is excluded from the flag-armed drop entry (`!isArray`), so nothing
// carries the lend through.
//
// *** THIS IS NOT EVIDENCE OF OWNERSHIP CORRECTNESS. ***
// liveCount is blind to arrays — see instrumentIsBlindToArrays, which
// measures a delta of 0 across an array allocation. A double free and a
// correct program are indistinguishable to this assertion. What it DOES
// pin is weaker and still worth having: the shape compiles, runs, reads
// back the right value, and does not abort. Settling the ownership
// question needs a canary or a sanitizer run (plan 2.2.7).
TEST(OwnershipRuntimeProbeTests, arrayBorrowShapeCompilesAndRuns) {
    std::string src =
        "package test;\n"
        "public final class Box {\n"
        "    int8[] data;\n"
        "    public Box() {\n"
        "        this.data = heap int8[4];\n"
        "        this.data[0] = (int8) 7;\n"
        "    }\n"
        "    public int8[] peek() { int8[] v = this.data; return v; }\n"
        "}\n"
        "public final class Sink {\n"
        "    int8[] held;\n"
        "    public Sink(#int8[] b) { this.held #= b; }\n"
        "    public int32 first() { return (int32) this.held[0]; }\n"
        "}\n"
        "public final class D {\n"
        "    static int32 work() {\n"
        "        Box b = heap Box();\n"
        "        int8[] v = b.peek();\n"    // LENT out of the container
        "        Sink s = heap Sink(#v);\n" // ...surrendered
        "        return s.first();\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int32 t = D.work();\n"
        "        int64 leaked = Cajeta.liveCount() - base;\n"
        "        return (int32) (leaked * 100) + t;\n"
        "    }\n"
        "}\n";
    // 7 == ran and read back correctly. The leaked term is structurally
    // always 0 here because the counter cannot see the array; it is left in
    // so this reads identically to probe 1 rather than looking like a
    // different, weaker measurement was intended.
    int32_t got = runI32(src);
    EXPECT_EQ(got, 7) << "array shape did not run cleanly (value=" << got
                      << "); note liveCount cannot see arrays, so this says "
                         "nothing about ownership either way";
}

// INSTRUMENT VALIDATION — liveCount() is BLIND TO ARRAYS. Measured, and
// pinned here so the array probe above is never mistaken for evidence.
//
// This ran first as `EXPECT_EQ(delta, 1)` and failed with delta == 0. That
// is the finding, not a defect: an untracked double free and a correct
// program produce the identical zero, so `arrayBorrowShapeCompilesAndRuns`
// cannot detect one. Asserting the observed value keeps the fact in the
// suite without leaving a permanently-red test.
//
// If arrays ever DO become tracked this test fails, which is the correct
// prompt: re-run the array shape with the now-capable instrument and settle
// the question left open in plan item 2.2.7.
TEST(OwnershipRuntimeProbeTests, instrumentIsBlindToArrays) {
    std::string src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int8[] a = heap int8[4];\n"
        "        a[0] = (int8) 1;\n"
        "        int64 delta = Cajeta.liveCount() - base;\n"
        "        return (int32) delta;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 0)
        << "liveCount now observes array allocations — the array ownership "
           "probe is no longer vacuous, so re-run it (plan 2.2.7)";
}

// The same, for a vtable-bearing CLASS — pins that the counter does observe
// the kind probe 1 measures, so probe 1's zero is meaningful.
TEST(OwnershipRuntimeProbeTests, instrumentSeesClassAllocation) {
    std::string src =
        "package test;\n"
        "public final class Cell {\n"
        "    public int32 n;\n"
        "    public Cell(int32 v) { this.n = v; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        Cell c = heap Cell(1);\n"
        "        int64 delta = Cajeta.liveCount() - base;\n"
        "        return (int32) delta + c.n - 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// CONTROL — the same class shape with the value TRANSFERRED out rather than
// lent. Balanced here proves the harness itself measures what it claims.
TEST(OwnershipRuntimeProbeTests, controlTransferredClassIsBalanced) {
    std::string src =
        "package test;\n"
        "public final class Cell {\n"
        "    public int32 n;\n"
        "    public Cell(int32 v) { this.n = v; }\n"
        "}\n"
        "public final class Holder {\n"
        "    Cell held;\n"
        "    public Holder() { this.held = null; }\n"
        "    public void put(#Cell v) { this.held #= v; }\n"
        "    public int32 value() { return this.held.n; }\n"
        "}\n"
        "public final class D {\n"
        "    static int32 work() {\n"
        "        Cell c = heap Cell(7);\n"
        "        Holder h = heap Holder();\n"
        "        h.put(#c);\n"
        "        return h.value();\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int32 t = D.work();\n"
        "        int64 leaked = Cajeta.liveCount() - base;\n"
        "        return (int32) (leaked * 100) + t;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}
