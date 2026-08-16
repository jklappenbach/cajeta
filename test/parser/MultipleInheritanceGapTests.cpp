//
// Multiple-inheritance gap tests (Features.md L-03 follow-up).
//
// Each test is currently DISABLED_ — strip the prefix when the
// corresponding ToDo.md "Priority 2 → 5 → item N" lands. The tests
// document concrete, reproducible source shapes that SHOULD work
// under the L-03 design (docs/specification/lang/UnifiedClasses.md
// § Inheritance) but currently fail.
//
// Gap surface confirmed by probe session 2026-05-17. See ToDo.md
// Priority 2 item 5 for the prioritized fix order and which row in
// Features.md each unblocks (S-107, S-302, S-305 mostly).
//
// gtest convention: tests starting with `DISABLED_` are filtered out
// of the default run, so the tree-green count is unaffected. Run
// them explicitly with:
//   ./cajeta_tests.sh '--gtest_also_run_disabled_tests --gtest_filter=MultipleInheritanceGapTests.*'
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

// --- Gap 1: implicit super-ctor chains ALL parents -------------------------
//
// Today only ONE parent's ctor runs. Method.cpp's implicit-super
// emission used to break after the first parent; the fix iterates
// every entry in `parent->getSuperClasses()`. The test uses a static
// observation counter to avoid entangling with the separate Gap 8
// multi-parent-instance-field-layout issue — parents that touch
// instance fields hit the per-parent slot-index baking problem,
// while a shared static counter is unaffected.
//
// Blocks S-107 because `Optional<T> extends Stream<T>,
// AbstractHashable<T>` would otherwise leave the AbstractHashable
// side's state uninitialized.

TEST(MultipleInheritanceGapTests, twoParentsBothCtorsRun) {
    auto src =
        "package test;\n"
        "public class Tally { public static int32 count = 0; }\n"
        "public class A { public A() { Tally.count = Tally.count + 10; } }\n"
        "public class B { public B() { Tally.count = Tally.count + 20; } }\n"
        "public class C extends A, B { public C() { return; } }\n"
        "public final class D {\n"
        "  public static int32 run() {\n"
        "    C c = heap C();\n"
        "    return Tally.count;\n"   // 30 if both A() and B() ran
        "  }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 30);
}

// --- Gap 2: multi-parent field layout / lookup (from child code) ---------
//
// Two parents, each contributing one field, written explicitly from
// the child's ctor (bypassing both the Gap 1 implicit-super issue and
// the Gap 8 parent-method/ctor slot-baking issue — writes here are
// generated in C's context so they correctly use C's slot indices).
// Pins the getFieldLlvmIndex fix in CajetaClass.h that walks the
// child's layout in the same DFS order as appendInherited.

TEST(MultipleInheritanceGapTests, twoParentsFieldReadBackThroughChildWrite) {
    auto src =
        "package test;\n"
        "public class A { public int32 a; public A() { return; } }\n"
        "public class B { public int32 b; public B() { return; } }\n"
        "public class C extends A, B {\n"
        "  public C() { this.a = 7; this.b = 11; }\n"
        "}\n"
        "public final class D {\n"
        "  public static int32 run() {\n"
        "    C c = heap C();\n"
        "    return c.a + c.b;\n"  // 18 if both slots are distinct and the
                                   //  ctor writes land on the right ones
        "  }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 18);
}

// --- Gap 3: diamond field dedup -------------------------------------------
//
// `appendInherited` (CajetaClass.cpp:288) is a naive recursive walk
// — for E extends B, C where both extend A, A's fields would be
// pushed twice. UnifiedClasses.md:176 claims "Common-ancestor state
// (A's fields) is shared, not duplicated"; this test discriminates
// dedup from duplicate-but-symmetric resolution by writing through
// `this.a` and reading the count of E's struct (via a side channel
// that touches both copies if duplicated).
//
// The probe-friendly version: assign different values through two
// methods that each name `this.a`, and check the final value is
// the LAST write. A duplicate-slot layout would either pick a
// different slot per method (final value depends on order) or
// silently drop one of the writes.

TEST(MultipleInheritanceGapTests, diamondCommonAncestorFieldShared) {
    auto src =
        "package test;\n"
        "public class A { public int32 a; public A() { return; } }\n"
        "public class B extends A { public B() { return; } }\n"
        "public class C extends A { public C() { return; } }\n"
        "public class E extends B, C {\n"
        "  public E() { return; }\n"
        "  public void setA(int32 v) { this.a = v; }\n"
        "  public int32 getA() { return this.a; }\n"
        "}\n"
        "public final class D {\n"
        "  public static int32 run() {\n"
        "    E e = heap E();\n"
        "    e.setA(42);\n"
        "    return e.getA();\n"   // 42 if A's field has exactly one slot;
                                  //  0 or garbage if setA and getA bind to
                                  //  different copies of A.a
        "  }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// --- Gap 4: abstract-method enforcement -----------------------------------
//
// Stream.cajeta:18 called this out: `next()` was "abstract via empty-
// default convention" because the compiler couldn't yet refuse a
// non-abstract subclass that fails to override an abstract base
// method. Fix had three parts:
//   1) visitMethodDeclaration in CajetaLlvmVisitor.h didn't accept the
//      `methodBody : ';'` alternative — it tried `any_cast<BlockPtr>`
//      on an empty std::any and threw bad_any_cast. Now: null block
//      when methodBody is `;`.
//   2) Same site sets `method->setAbstract(true)` for body-less methods
//      so isAbstract() returns true (previously only interface methods
//      were flagged).
//   3) CajetaClass::buildVirtualTable raises
//      CAJETA_ERROR_ABSTRACT_NOT_IMPLEMENTED when this class has no
//      abstract methods of its own (heuristic: "self is concrete")
//      and any inherited abstract lacks a concrete override.

TEST(MultipleInheritanceGapTests, unimplementedAbstractMethodRejected) {
    // Concrete subclass `Bad` does NOT override `must()`. Compile
    // should fail with the specific error id.
    auto src =
        "package test;\n"
        "public abstract class Base {\n"
        "  public abstract int32 must();\n"
        "}\n"
        "public class Bad extends Base {\n"
        "  public Bad() { return; }\n"
        "}\n"
        "public final class D {\n"
        "  public static int32 run() {\n"
        "    Bad b = heap Bad();\n"
        "    return b.must();\n"
        "  }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.D");
        FAIL() << "expected CAJETA_ERROR_ABSTRACT_NOT_IMPLEMENTED";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_ABSTRACT_NOT_IMPLEMENTED");
    }
}

// Companion positive case for Gap 4 — the same shape but with the
// override present must compile and dispatch normally. Pre-fix this
// also threw (the bad_any_cast from visitMethodDeclaration fired
// regardless of subclass content), so a clean pass here is the real
// signal that the fix is correct rather than incidental.

TEST(MultipleInheritanceGapTests, abstractMethodWithOverrideCompilesAndDispatches) {
    auto src =
        "package test;\n"
        "public abstract class Base {\n"
        "  public abstract int32 must();\n"
        "}\n"
        "public class Good extends Base {\n"
        "  public Good() { return; }\n"
        "  public int32 must() { return 99; }\n"
        "}\n"
        "public final class D {\n"
        "  public static int32 run() {\n"
        "    Good g = heap Good();\n"
        "    return g.must();\n"
        "  }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 99);
}

// --- Gap 5: super.method() upcall ------------------------------------------
//
// SuperExpression lands as UnsupportedExpression today
// (Expression.cpp:66,790). Needed when an override wants to delegate
// to the parent's body. Also tagged as L-19 in Features.md.

TEST(MultipleInheritanceGapTests, superMethodCallReachesParent) {
    auto src =
        "package test;\n"
        "public class Animal {\n"
        "  public int32 speak() { return 10; }\n"
        "}\n"
        "public class Dog extends Animal {\n"
        "  public int32 speak() { return super.speak() + 32; }\n"
        "}\n"
        "public final class D {\n"
        "  public static int32 run() {\n"
        "    Dog d = heap Dog();\n"
        "    return d.speak();\n"   // 42 — Dog::speak adds 32 to Animal::speak
        "  }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// --- Gap 6: explicit super(args) in ctor -----------------------------------
//
// Grammar accepts `SUPER '(' parameterList? ')'` (CajetaParser.g4:630)
// but the visitor lowers it to UnsupportedExpression("super call").
// Needed whenever a parent class doesn't have a no-arg ctor.

TEST(MultipleInheritanceGapTests, explicitSuperCtorWithArgs) {
    auto src =
        "package test;\n"
        "public class P {\n"
        "  public int32 seeded;\n"
        "  public P(int32 v) { this.seeded = v; }\n"
        "}\n"
        "public class C extends P {\n"
        "  public C() { super(99); }\n"
        "}\n"
        "public final class D {\n"
        "  public static int32 run() {\n"
        "    C c = heap C();\n"
        "    return c.seeded;\n"   // 99
        "  }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 99);
}

// Regression: `super(param)` where `param` is one of the child ctor's
// formal parameters. Before the loadIfLValue addition in
// MethodCallExpression's super-ctor branch, the alloca address for
// `param` was passed to the parent ctor — JIT verify tripped with
// "Call parameter type does not match function signature!" because
// the parent's signature wanted i32 (the loaded value), not ptr.
TEST(MultipleInheritanceGapTests, explicitSuperCtorWithParameterArg) {
    auto src =
        "package test;\n"
        "public class P {\n"
        "  public int32 seeded;\n"
        "  public P(int32 v) { this.seeded = v; }\n"
        "}\n"
        "public class C extends P {\n"
        "  public int32 c;\n"
        "  public C(int32 bv, int32 cv) { super(bv); this.c = cv; }\n"
        "}\n"
        "public final class D {\n"
        "  public static int32 run() {\n"
        "    C c = heap C(3, 7);\n"
        "    return c.seeded * 10 + c.c;\n"
        "  }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 37);
}

// --- Gap 7: unsatisfied-interface enforcement ------------------------------
//
// CajetaClass.cpp:1247 — interface methods with no concrete impl on
// the implementing class are silently skipped: *"today we skip and
// the vtable simply omits the entry"*. The implementing class
// compiles successfully and calls through the interface produce
// garbage / null dispatch. Should raise
// CAJETA_ERROR_INTERFACE_NOT_IMPLEMENTED at prototype build.

TEST(MultipleInheritanceGapTests, unimplementedInterfaceMethodRejected) {
    auto src =
        "package test;\n"
        "public interface Speaker {\n"
        "  public int32 speak();\n"
        "}\n"
        "public class Mute implements Speaker {\n"   // doesn't implement speak()
        "  public Mute() { return; }\n"
        "}\n"
        "public final class D {\n"
        "  public static int32 run() {\n"
        "    Mute m = heap Mute();\n"
        "    return 0;\n"
        "  }\n"
        "}\n";
    EXPECT_ANY_THROW(CajetaJit::compile(src, "test.D"));
}

// --- Gap 8: multi-parent ctor/method field-slot baking --------------------
//
// The "real" version of Gap 1. Before the sub-object layout fix:
//   - A is compiled standalone — A.ctor's IR writes A.first at slot 1
//     of A's own struct.
//   - B is compiled standalone — B.ctor's IR writes B.first at slot 1
//     of B's own struct.
//   - C extends A, B has layout { vtable, A.first, B.first, C.own... }.
//     Calling A.ctor(C*) works (slot 1 still hits A.first). Calling
//     B.ctor(C*) ALSO writes to slot 1 → clobbers A.first, leaves
//     B.first uninitialized.
// Fixed by per-parent sub-object layout + this-offset adjustment at
// every parent ctor/method call site.


// Companion: parent METHOD (not just ctor) reads parent's own field via
// `this` after a write in the child. Exercises the parent-method
// receiver-adjustment path independently from the ctor path.

TEST(MultipleInheritanceGapTests, parentMethodReadsOwnFieldOnSubclassInstance) {
    auto src =
        "package test;\n"
        "public class A {\n"
        "  public int32 a;\n"
        "  public A() { return; }\n"
        "  public int32 getA() { return this.a; }\n"
        "}\n"
        "public class B {\n"
        "  public int32 b;\n"
        "  public B() { return; }\n"
        "  public int32 getB() { return this.b; }\n"
        "}\n"
        "public class C extends A, B {\n"
        "  public C() { this.a = 4; this.b = 38; }\n"
        "}\n"
        "public final class D {\n"
        "  public static int32 run() {\n"
        "    C c = heap C();\n"
        "    return c.getA() + c.getB();\n"   // 42 if each parent method
                                              // receives the correctly
                                              // adjusted sub-object ptr
        "  }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// Two-field-per-parent variant — guards against off-by-one in the sub-
// object slot map (e.g., if only the first slot of each parent is
// correctly addressed but the second one drifts).

TEST(MultipleInheritanceGapTests, twoParentsMultipleFieldsEach) {
    auto src =
        "package test;\n"
        "public class A {\n"
        "  public int32 a1; public int32 a2;\n"
        "  public A() { this.a1 = 1; this.a2 = 2; }\n"
        "}\n"
        "public class B {\n"
        "  public int32 b1; public int32 b2;\n"
        "  public B() { this.b1 = 4; this.b2 = 8; }\n"
        "}\n"
        "public class C extends A, B { public C() { return; } }\n"
        "public final class D {\n"
        "  public static int32 run() {\n"
        "    C c = heap C();\n"
        "    return c.a1 + c.a2 + c.b1 + c.b2;\n"   // 15
        "  }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 15);
}

// --- Polymorphic dispatch through a non-first-parent-typed binding -------
//
// After Gap 8 the per-parent sub-object layout exists, but polymorphism
// through a B-typed binding to a C instance (where C extends A, B and
// B is the SECOND parent) still doesn't work:
//   (1) the assignment `B b = c` keeps `b == c` and never adjusts the
//       pointer to B's sub-object inside c. b[0] then reads C's primary
//       vtable, and the only "B method" the vtable resolves through
//       hash lookup is bound to the WRONG `this` (it expects a B-shaped
//       view starting at b[0]).
//   (2) even if we adjusted the pointer, the secondary vptr slot at
//       offsetof(B in C) is currently zeroed (CreatorRest only writes
//       slot 0). A dispatch attempt would null-deref.
//   (3) for overrides — `class C extends A, B { int32 getKind() { ... } }
//       overriding B.getKind` — the secondary vtable would need to point
//       at a thunk that adjusts `this` back to C* before tail-calling
//       C::getKind.
// These three tests pin the three pieces.

TEST(MultipleInheritanceGapTests, dispatchThroughNonFirstParentTypedBinding) {
    auto src =
        "package test;\n"
        "public class A {\n"
        "  public int32 a;\n"
        "  public A() { this.a = 7; }\n"
        "  public int32 getA() { return this.a; }\n"
        "}\n"
        "public class B {\n"
        "  public int32 b;\n"
        "  public B() { this.b = 13; }\n"
        "  public int32 getB() { return this.b; }\n"
        "}\n"
        "public class C extends A, B {\n"
        "  public C() { return; }\n"
        "}\n"
        "public final class D {\n"
        "  public static int32 run() {\n"
        "    C c = heap C();\n"
        "    B b = c;\n"          // upcast to non-first parent
        "    return b.getB();\n"  // 13 — B's getB sees B's sub-object's `b` slot
        "  }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 13);
}

TEST(MultipleInheritanceGapTests, overrideThroughNonFirstParentTypedBinding) {
    auto src =
        "package test;\n"
        "public class A {\n"
        "  public A() { return; }\n"
        "  public int32 kind() { return 1; }\n"
        "}\n"
        "public class B {\n"
        "  public B() { return; }\n"
        "  public int32 kind() { return 2; }\n"
        "}\n"
        "public class C extends A, B {\n"
        "  public int32 marker;\n"
        "  public C() { this.marker = 99; }\n"
        "  public int32 kind() { return this.marker; }\n"   // override
        "}\n"
        "public final class D {\n"
        "  public static int32 run() {\n"
        "    C c = heap C();\n"
        "    B b = c;\n"             // upcast to second parent
        "    return b.kind();\n"     // 99 — override fires through secondary vtable
        "  }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 99);
}

// Sanity: with first-parent binding the existing single-inheritance path
// must keep working. This is a regression guard for the upcast change —
// a first-parent upcast must NOT inject any byte adjustment.

TEST(MultipleInheritanceGapTests, dispatchThroughFirstParentTypedBindingUnchanged) {
    auto src =
        "package test;\n"
        "public class A {\n"
        "  public int32 a;\n"
        "  public A() { this.a = 7; }\n"
        "  public int32 getA() { return this.a; }\n"
        "}\n"
        "public class B {\n"
        "  public int32 b;\n"
        "  public B() { this.b = 13; }\n"
        "  public int32 getB() { return this.b; }\n"
        "}\n"
        "public class C extends A, B {\n"
        "  public C() { return; }\n"
        "}\n"
        "public final class D {\n"
        "  public static int32 run() {\n"
        "    C c = heap C();\n"
        "    A a = c;\n"           // upcast to FIRST parent; offset 0
        "    return a.getA();\n"   // 7 — primary vtable, no adjustment
        "  }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}
