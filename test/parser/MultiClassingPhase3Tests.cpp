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
//   - D's own code via `this.x` / `this[A].x` reads + writes the
//     single shared A consistently.
//   - First-parent inherited methods that touch A internally via
//     `this.x` work because the first parent's standalone-inline-A
//     position IS the canonical position.
//
// What v1 DOES NOT ship (Phase 3 v2 — requires full vbase ABI):
//   - `this[NonFirstParent].sharedAncestorField` access: the non-
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

// `this[A].x` from D's own method must hit the same shared storage
// as `this.x`. Pre-fix, `this[A]` adjusted to subObjectSlotMap[A]
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
        "    this[A].x = 17;\n"
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
        "    super[B].setX(99);\n"  // calls A's setX via B's first-parent chain
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

// --- Phase 3 v2 limitations (DISABLED_) -----------------------------------
//
// `this[C].x` where x is a shared-ancestor field: C is the NON-first
// parent of Diamond. With v1 dedup, C's sub-object in Diamond has no
// inline A — GEPing through C's standalone struct type lands on the
// wrong memory. Fixing properly requires vbase machinery (every
// multi-parent class gains a vbase pointer per inherited parent so
// shared-ancestor access can indirect through a runtime-loaded
// pointer). Strip DISABLED_ when Phase 3 v2 ships.

TEST(MultiClassingPhase3Tests,
        DISABLED_thisBracketNonFirstParentReadsSharedAncestorField) {
    auto src =
        "package test;\n"
        "public class A { public int32 x; public A() { return; } }\n"
        "public class B extends A { public B() { return; } }\n"
        "public class C extends A { public C() { return; } }\n"
        "public class Diamond extends B, C {\n"
        "  public Diamond() { this.x = 55; }\n"
        "  public int32 readViaC() {\n"
        "    return this[C].x;\n"  // v1: WRONG memory; v2: 55
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
