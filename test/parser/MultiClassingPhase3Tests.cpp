//
// MultiClassing Phase 3 — shared-diamond layout dedup (v1).
//
// Design: cajeta-docs/stdlib/MultiClassing.md § P-4 + § Phase 3.
//
// Scope of v1: when D extends B, C and both B, C extend A, D's
// layout emits A's content EXACTLY ONCE. The single shared A lives
// at the canonical offset (first-parent's-chain emission); both B's
// and C's sub-objects in D refer back to that single A.
//
// What v1 ships:
//   - Layout dedup: D has one A, not two.
//   - `subObjectSlotMap[A]` records the canonical offset (first
//     emission), so `adjustForUpcast(this, D, A)` lands on the
//     shared A regardless of which path the upcast traversed.
//   - `getFieldLlvmIndex(A.x)` returns the canonical slot.
//   - D's own code via `this.x` / `this<A>.x` reads + writes the
//     single shared A consistently.
//   - First-parent inherited methods that touch A internally via
//     `this.x` work because the first parent's standalone-inline-A
//     position IS the canonical position.
//
// What v1 DOES NOT ship (Phase 3 v2 — requires full vbase ABI):
//   - `this<NonFirstParent>.sharedAncestorField` access: the non-
//     first parent's sub-object in D no longer contains the shared
//     ancestor's inline content (deduped away), so GEPing through
//     that parent's standalone struct type would land on wrong
//     memory.
//   - Inherited methods on non-first parents accessing shared-
//     ancestor fields via inline `this.x`: same root cause — the
//     non-first parent's compiled IR assumes inline storage.
//
// Both v2 limitations are tracked under ToDo.md Priority 1 as the
// Phase 3 v2 follow-up. The DISABLED_ test below pins one of them.
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

// --- D's own code on a shared ancestor ------------------------------------
//
// `this.x` resolves via the field-walk to A.x (the single shared
// declaration). With layout dedup, `getFieldLlvmIndex(A.x)` returns
// the canonical slot — D writes to and reads from the same storage.

TEST(MultiClassingPhase3Tests, dCodeWritesAndReadsSharedAncestorViaThis) {
    auto src =
        "package test;\n"
        "public class A { public int32 x; public A() { return; } }\n"
        "public class B extends A { public B() { return; } }\n"
        "public class C extends A { public C() { return; } }\n"
        "public class D extends B, C {\n"
        "  public D() { this.x = 0; }\n"
        "  public int32 setAndRead() {\n"
        "    this.x = 42;\n"
        "    return this.x;\n"  // 42 — single-storage round trip
        "  }\n"
        "}\n"
        "public final class D {\n"
        "  public static int32 run() {\n"
        "    test.D d = new test.D();\n"
        "    return d.setAndRead();\n"
        "  }\n"
        "}\n";
    // Note: the outer `public final class D` shadows the inner test.D for the
    // CompilerTests' entry point; we resolve via the package-qualified name in
    // the body. To avoid the name clash, the entry class is named differently
    // below.
    auto srcOk =
        "package test;\n"
        "public class A { public int32 x; public A() { return; } }\n"
        "public class B extends A { public B() { return; } }\n"
        "public class C extends A { public C() { return; } }\n"
        "public class Diamond extends B, C {\n"
        "  public Diamond() { this.x = 0; }\n"
        "  public int32 setAndRead() {\n"
        "    this.x = 42;\n"
        "    return this.x;\n"
        "  }\n"
        "}\n"
        "public final class D {\n"
        "  public static int32 run() {\n"
        "    Diamond d = new Diamond();\n"
        "    return d.setAndRead();\n"
        "  }\n"
        "}\n";
    EXPECT_EQ(runI32(srcOk), 42);
}

// `this<A>.x` from D's own method must hit the same shared storage
// as `this.x`. Pre-fix, `this<A>` adjusted to subObjectSlotMap[A]
// = the LAST-recorded position (C's chain inline A), which differed
// from `this.x`'s canonical (first-emitted) A.x slot. After fix,
// both forms reach the single canonical storage.

TEST(MultiClassingPhase3Tests, dCodeWritesViaThisBracketAReadsViaThis) {
    auto src =
        "package test;\n"
        "public class A { public int32 x; public A() { return; } }\n"
        "public class B extends A { public B() { return; } }\n"
        "public class C extends A { public C() { return; } }\n"
        "public class Diamond extends B, C {\n"
        "  public Diamond() { return; }\n"
        "  public int32 viaBracketThenViaPlain() {\n"
        "    this<A>.x = 17;\n"
        "    return this.x;\n"  // 17 if both forms reach same storage
        "  }\n"
        "}\n"
        "public final class D {\n"
        "  public static int32 run() {\n"
        "    Diamond d = new Diamond();\n"
        "    return d.viaBracketThenViaPlain();\n"
        "  }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 17);
}

// First-parent inherited method writes to A.x via the parent's
// standalone-compiled IR (which assumes A inline at parent's slot 1).
// Because B is the FIRST parent of Diamond and B's standalone layout
// IS the canonical layout for Diamond's primary chain, B's IR lands
// on the canonical shared A. Reading back through Diamond's own
// `this.x` returns the same value.

TEST(MultiClassingPhase3Tests, firstParentInheritedMethodReachesSharedA) {
    auto src =
        "package test;\n"
        "public class A {\n"
        "  public int32 x;\n"
        "  public A() { return; }\n"
        "  public void setX(int32 v) { this.x = v; }\n"
        "}\n"
        "public class B extends A { public B() { return; } }\n"
        "public class C extends A { public C() { return; } }\n"
        "public class Diamond extends B, C {\n"
        "  public Diamond() { return; }\n"
        "  public int32 useInheritedSetter() {\n"
        "    super<B>.setX(99);\n"  // calls A's setX via B's first-parent chain
        "    return this.x;\n"  // 99 — same storage
        "  }\n"
        "}\n"
        "public final class D {\n"
        "  public static int32 run() {\n"
        "    Diamond d = new Diamond();\n"
        "    return d.useInheritedSetter();\n"
        "  }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 99);
}

// The shared ancestor's field name is declared exactly once (on A),
// so Phase 1's sibling-collision check must NOT fire on `this.x` in
// Diamond even though there are TWO paths (B and C) reaching A. The
// check already skips when both gathered entries share a declaring
// class (the existing `allMatches[i].first == allMatches[j].first`
// short-circuit), but pin the behavior in case future refactors
// drift.

TEST(MultiClassingPhase3Tests, diamondFieldDoesNotFireAmbiguityCheck) {
    auto src =
        "package test;\n"
        "public class A { public int32 x; public A() { return; } }\n"
        "public class B extends A { public B() { return; } }\n"
        "public class C extends A { public C() { return; } }\n"
        "public class Diamond extends B, C {\n"
        "  public Diamond() { return; }\n"
        "  public int32 readX() { return this.x; }\n"  // would throw if ambiguity fired
        "}\n"
        "public final class D {\n"
        "  public static int32 run() {\n"
        "    Diamond d = new Diamond();\n"
        "    return d.readX();\n"  // 0 (field uninitialized by default)
        "  }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 0);
}

// Diamond fields with explicit initialization through D's own ctor
// hold across multiple method calls — verifies the shared storage
// isn't accidentally zeroed by some secondary-ctor path.

TEST(MultiClassingPhase3Tests, diamondFieldSurvivesMultipleAccesses) {
    auto src =
        "package test;\n"
        "public class A { public int32 x; public A() { return; } }\n"
        "public class B extends A { public B() { return; } }\n"
        "public class C extends A { public C() { return; } }\n"
        "public class Diamond extends B, C {\n"
        "  public Diamond() { this.x = 100; }\n"
        "  public int32 first() { return this.x; }\n"
        "  public int32 second() { return this.x; }\n"
        "}\n"
        "public final class D {\n"
        "  public static int32 run() {\n"
        "    Diamond d = new Diamond();\n"
        "    return d.first() + d.second();\n"  // 200
        "  }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 200);
}

// --- Phase 3 v2 — cross-path bracketed access via diamond -----------------
//
// `this<C>.x` where x is a shared-ancestor field: C is the NON-first
// parent of Diamond. v1 returned the wrong memory because GEPing
// through C's standalone struct type lands on C's dormant inline-A
// (storage exists but is never written by any code).
//
// v2 fix: in DotExpression, when the property's declaring class is
// reachable via multiple offsets from the enclosing class (canonical
// offset != via-LHS-path offset), shift `base` back toward the
// canonical position and GEP through declaringClass's struct type
// at the property's declaringClass-standalone slot. Result: both
// `this<B>.x` and `this<C>.x` reach the canonical shared A.x.

TEST(MultiClassingPhase3Tests,
        thisBracketNonFirstParentReadsSharedAncestorField) {
    auto src =
        "package test;\n"
        "public class A { public int32 x; public A() { return; } }\n"
        "public class B extends A { public B() { return; } }\n"
        "public class C extends A { public C() { return; } }\n"
        "public class Diamond extends B, C {\n"
        "  public Diamond() { this.x = 55; }\n"
        "  public int32 readViaC() {\n"
        "    return this<C>.x;\n"  // canonical A.x = 55
        "  }\n"
        "}\n"
        "public final class D {\n"
        "  public static int32 run() {\n"
        "    Diamond d = new Diamond();\n"
        "    return d.readViaC();\n"
        "  }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 55);
}

// Symmetric write path — `this<C>.x = N` must also reach the
// canonical A.x storage so a subsequent `this.x` read returns N.

TEST(MultiClassingPhase3Tests,
        thisBracketNonFirstParentWritesSharedAncestorField) {
    auto src =
        "package test;\n"
        "public class A { public int32 x; public A() { return; } }\n"
        "public class B extends A { public B() { return; } }\n"
        "public class C extends A { public C() { return; } }\n"
        "public class Diamond extends B, C {\n"
        "  public Diamond() { return; }\n"
        "  public int32 writeViaCReadViaThis() {\n"
        "    this<C>.x = 73;\n"
        "    return this.x;\n"  // 73 if both reach canonical storage
        "  }\n"
        "}\n"
        "public final class D {\n"
        "  public static int32 run() {\n"
        "    Diamond d = new Diamond();\n"
        "    return d.writeViaCReadViaThis();\n"
        "  }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 73);
}

// And the round trip through both bracketed forms — `this<B>.x` and
// `this<C>.x` should read the SAME storage after either writes.
// Pre-v2: `this<B>.x` and `this<C>.x` returned independent values.

TEST(MultiClassingPhase3Tests,
        thisBracketBothPathsSeeSameSharedAncestorStorage) {
    auto src =
        "package test;\n"
        "public class A { public int32 x; public A() { return; } }\n"
        "public class B extends A { public B() { return; } }\n"
        "public class C extends A { public C() { return; } }\n"
        "public class Diamond extends B, C {\n"
        "  public Diamond() { return; }\n"
        "  public int32 crossPathRoundTrip() {\n"
        "    this<B>.x = 11;\n"  // write via first parent's view
        "    int32 viaC = this<C>.x;\n"  // read via second parent's view
        "    this<C>.x = viaC + 6;\n"  // 11 + 6 = 17
        "    return this<B>.x;\n"  // 17 if both forms reach same storage
        "  }\n"
        "}\n"
        "public final class D {\n"
        "  public static int32 run() {\n"
        "    Diamond d = new Diamond();\n"
        "    return d.crossPathRoundTrip();\n"
        "  }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 17);
}

// Sanity: the v2 routing must NOT fire for non-diamond first-parent
// access. `this<B>.x` in a non-diamond class graph should still work
// via the normal (un-rerouted) path. (B is C's first parent here, so
// no diamond exists.)

TEST(MultiClassingPhase3Tests, thisBracketFirstParentNoDiamondStillWorks) {
    auto src =
        "package test;\n"
        "public class A { public int32 x; public A() { return; } }\n"
        "public class B extends A { public B() { return; } }\n"
        "public class C extends B {\n"  // single-inheritance chain: C -> B -> A
        "  public C() { this<A>.x = 42; }\n"
        "  public int32 read() { return this<B>.x; }\n"  // x is from A, reachable through B (single path)
        "}\n"
        "public final class D {\n"
        "  public static int32 run() {\n"
        "    C c = new C();\n"
        "    return c.read();\n"  // 42
        "  }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// --- Phase 3 v3 — inherited-method re-adjustment via diamond --------------
//
// `super<C>.setX(88)` where `setX` is INHERITED from a shared ancestor
// A: the method's actual declaring class is A, not C. SuperExpression
// adjusted `this` to C's sub-object (dormant inline-A in Diamond),
// and pre-v3 the dispatch called setX with that adjusted pointer —
// setX wrote A.x via C's standalone slot, hitting dormant memory.
//
// v3 fix: in MCE's super-dispatch path, when the resolved method's
// declaring class differs from the bracketed class AND a diamond
// exists, re-adjust `thisValue` from the bracketed position to the
// declaring class's canonical position. For super<C>.setX in Diamond:
// declaring class = A, canonical A in Diamond = 0, via-C-path = 16,
// delta = -16, `this` lands back on Diamond's canonical A. The
// inherited setX function (unchanged, expects A-pointer) reads/writes
// the right storage.

TEST(MultiClassingPhase3Tests,
        inheritedMethodOnNonFirstParentReachesSharedA) {
    auto src =
        "package test;\n"
        "public class A {\n"
        "  public int32 x;\n"
        "  public A() { return; }\n"
        "  public void setX(int32 v) { this.x = v; }\n"
        "}\n"
        "public class B extends A { public B() { return; } }\n"
        "public class C extends A { public C() { return; } }\n"
        "public class Diamond extends B, C {\n"
        "  public Diamond() { return; }\n"
        "  public int32 setViaCReadViaThis() {\n"
        "    super<C>.setX(88);\n"  // C's IR writes to dormant inline-A
        "    return this.x;\n"  // v2: 0 (dormant); v3: 88 (canonical)
        "  }\n"
        "}\n"
        "public final class D {\n"
        "  public static int32 run() {\n"
        "    Diamond d = new Diamond();\n"
        "    return d.setViaCReadViaThis();\n"
        "  }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 88);
}

// --- v4: non-first parent's OWN method touching shared-ancestor field ----
//
// Tighter shape than v3: the touching method (`cWritesSharedX`) is
// declared ON C, not inherited from A. So declaringClass == bracketed
// class == C, and v3's "differs → re-adjust" check doesn't fire. C's
// IR was compiled standalone with the assumption that `this` is a
// C-pointer and inline-A sits at offset 8 (after vtable). When called
// from Diamond, `this` arrives as Diamond + offsetOfCInDiamond, and
// C's IR's GEP for `this.x` lands on the dormant inline-A — not the
// canonical (B-chain) A.
//
// Out of scope for v1/v2/v3. Two viable structural fixes captured in
// ToDo.md (vbase ABI vs per-descendant recompilation); this test pins
// the gap so whichever lands can flip it green.
TEST(MultiClassingPhase3Tests,
        ownMethodOnNonFirstParentReachesSharedA) {
    auto src =
        "package test;\n"
        "public class A {\n"
        "  public int32 x;\n"
        "  public A() { return; }\n"
        "}\n"
        "public class B extends A { public B() { return; } }\n"
        "public class C extends A {\n"
        "  public C() { return; }\n"
        "  public void cWritesSharedX(int32 v) { this.x = v; }\n"
        "}\n"
        "public class Diamond extends B, C {\n"
        "  public Diamond() { return; }\n"
        "  public int32 callCThenReadShared() {\n"
        "    super<C>.cWritesSharedX(99);\n"  // C-own method, GEPs into C-inline-A
        "    return this.x;\n"  // v3 returns 0 (dormant); v4 expects 99
        "  }\n"
        "}\n"
        "public final class D {\n"
        "  public static int32 run() {\n"
        "    Diamond d = new Diamond();\n"
        "    return d.callCThenReadShared();\n"
        "  }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 99);
}

// Full v4 (a): C's method touches BOTH own and inherited fields. The
// narrow trick disqualifies (single `this` adjustment can't satisfy
// both); the vbase ABI handles it because inherited-field access
// loads through vbase while own-field access stays as direct GEP.
TEST(MultiClassingPhase3Tests,
        ownMethodOnNonFirstParentTouchesBothOwnAndInheritedFields) {
    auto src =
        "package test;\n"
        "public class A {\n"
        "  public int32 x;\n"
        "  public A() { return; }\n"
        "}\n"
        "public class B extends A { public B() { return; } }\n"
        "public class C extends A {\n"
        "  public int32 cOwn;\n"
        "  public C() { return; }\n"
        "  public void cWritesBoth(int32 v) {\n"
        "    this.x = v;\n"      // inherited from A
        "    this.cOwn = v;\n"   // C's own
        "  }\n"
        "}\n"
        "public class Diamond extends B, C {\n"
        "  public Diamond() { return; }\n"
        "  public int32 callCThenReadShared() {\n"
        "    super<C>.cWritesBoth(77);\n"
        "    return this.x;\n"
        "  }\n"
        "}\n"
        "public final class D {\n"
        "  public static int32 run() {\n"
        "    Diamond d = new Diamond();\n"
        "    return d.callCThenReadShared();\n"
        "  }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 77);
}

// Full v4 (b): C's method calls another method on `this`. The narrow
// trick disqualifies (the callee's vtable load would see the wrong
// vtable). The vbase ABI handles it because the callee's IR is itself
// vbase-aware — `this`-pointer adjustment is irrelevant for shared-
// ancestor access.
TEST(MultiClassingPhase3Tests,
        ownMethodOnNonFirstParentCallsThisHelper) {
    auto src =
        "package test;\n"
        "public class A {\n"
        "  public int32 x;\n"
        "  public A() { return; }\n"
        "}\n"
        "public class B extends A { public B() { return; } }\n"
        "public class C extends A {\n"
        "  public C() { return; }\n"
        "  public void cHelperWrite(int32 v) { this.x = v; }\n"
        "  public void cCallsHelper(int32 v) { this.cHelperWrite(v); }\n"
        "}\n"
        "public class Diamond extends B, C {\n"
        "  public Diamond() { return; }\n"
        "  public int32 callCThenReadShared() {\n"
        "    super<C>.cCallsHelper(55);\n"
        "    return this.x;\n"
        "  }\n"
        "}\n"
        "public final class D {\n"
        "  public static int32 run() {\n"
        "    Diamond d = new Diamond();\n"
        "    return d.callCThenReadShared();\n"
        "  }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 55);
}
