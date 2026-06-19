//
// MultiClassing Phase 2 — `super<Base>.method()` + `this<Base>.field`.
//
// Design: docs/specification/lang/MultiClassing.md § P-2 + § Phase 2.
//
// The bracket-form parent-view selector lets a child whose parents
// declare colliding names resolve the ambiguity by qualifying the
// access. `super<A>.kind()` direct-calls A's body (bypassing vtable).
// `this<A>.x` reads/writes the A sub-object's slot.
//
// These tests pair with `MultiClassingPhase1Tests.cpp` — the
// negative cases there throw without these escape hatches; the
// positive cases here demonstrate the user can take the path the
// error messages recommend.
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

// --- super<Base>.method() — direct call to selected parent's body ----------
//
// A and B both declare `kind()`. C overrides and combines their
// values via super<A>.kind() + super<B>.kind(). The override
// resolves the Phase 1 ambiguity (Phase 1 has the negative pair).

TEST(MultiClassingPhase2Tests, superBaseCombinesSiblingParentImpls) {
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
        "  public C() { return; }\n"
        "  public int32 kind() { return super<A>.kind() + super<B>.kind(); }\n"
        "}\n"
        "public final class D {\n"
        "  public static int32 run() {\n"
        "    C c = heap C();\n"
        "    return c.kind();\n"  // 1 + 2 = 3
        "  }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 3);
}

// super<B> in single inheritance is equivalent to super (only one
// parent). Sanity check that the bracket form is permitted when
// there's no ambiguity to disambiguate.

TEST(MultiClassingPhase2Tests, superBaseInSingleInheritanceMatchesPlainSuper) {
    auto src =
        "package test;\n"
        "public class Animal {\n"
        "  public Animal() { return; }\n"
        "  public int32 speak() { return 10; }\n"
        "}\n"
        "public class Dog extends Animal {\n"
        "  public Dog() { return; }\n"
        "  public int32 speak() { return super<Animal>.speak() + 32; }\n"
        "}\n"
        "public final class D {\n"
        "  public static int32 run() {\n"
        "    Dog d = heap Dog();\n"
        "    return d.speak();\n"  // 10 + 32 = 42
        "  }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// super<B> direct-call means: this+offset(B), then call B's body
// WITHOUT vtable lookup. If we accidentally went through the
// vtable, the override on the most-derived class would loop the
// call back to itself. (Pre-existing super.foo() test catches this
// for the single-parent case; the bracket form needs its own check
// since the dispatch goes through CajetaClass::invokeMethod with a
// different target class.)

TEST(MultiClassingPhase2Tests, superBaseBypassesVtableNoInfiniteLoop) {
    auto src =
        "package test;\n"
        "public class A {\n"
        "  public A() { return; }\n"
        "  public int32 step() { return 5; }\n"
        "}\n"
        "public class B {\n"
        "  public B() { return; }\n"
        "  public int32 step() { return 9; }\n"
        "}\n"
        "public class C extends A, B {\n"
        "  public C() { return; }\n"
        "  public int32 step() { return super<A>.step() * 2; }\n"  // would infinite-loop if vtable dispatched
        "}\n"
        "public final class D {\n"
        "  public static int32 run() {\n"
        "    C c = heap C();\n"
        "    return c.step();\n"  // 5 * 2 = 10
        "  }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 10);
}

// --- this<Base>.field — read/write the selected parent's slot --------------
//
// A and B both declare `total`. C uses this<A>.total / this<B>.total
// to reach each independent slot. Phase 1's ambiguity error is
// resolved by qualifying the access.

TEST(MultiClassingPhase2Tests, thisBaseReadsAndWritesIndependentParentSlots) {
    auto src =
        "package test;\n"
        "public class A { public int32 total; public A() { return; } }\n"
        "public class B { public int32 total; public B() { return; } }\n"
        "public class C extends A, B {\n"
        "  public C() {\n"
        "    this<A>.total = 10;\n"
        "    this<B>.total = 20;\n"
        "  }\n"
        "  public int32 sum() { return this<A>.total + this<B>.total; }\n"
        "}\n"
        "public final class D {\n"
        "  public static int32 run() {\n"
        "    C c = heap C();\n"
        "    return c.sum();\n"  // 10 + 20 = 30
        "  }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 30);
}

// Sanity: this<B> in single inheritance is equivalent to this for B
// fields. No ambiguity, but the syntax should still parse and
// resolve correctly so users have one consistent form.

TEST(MultiClassingPhase2Tests, thisBaseInSingleInheritanceWorks) {
    auto src =
        "package test;\n"
        "public class Counter {\n"
        "  public int32 n;\n"
        "  public Counter() { this.n = 0; }\n"
        "}\n"
        "public class Bumped extends Counter {\n"
        "  public Bumped() {\n"
        "    this<Counter>.n = 7;\n"
        "  }\n"
        "  public int32 read() { return this<Counter>.n; }\n"
        "}\n"
        "public final class D {\n"
        "  public static int32 run() {\n"
        "    Bumped b = heap Bumped();\n"
        "    return b.read();\n"  // 7
        "  }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

// --- Error cases ---------------------------------------------------------
//
// `super<X>` / `this<X>` where X is not an ancestor of the current
// class must reject at resolution time with CAJETA_ERROR_NOT_AN_ANCESTOR.
// This catches typos and mis-spelled class names before they cause
// silent miscompiles via the upcast machinery.

TEST(MultiClassingPhase2Tests, superBaseOnNonAncestorRejected) {
    auto src =
        "package test;\n"
        "public class A { public A() { return; } public int32 fn() { return 1; } }\n"
        "public class Unrelated { public Unrelated() { return; } }\n"
        "public class C extends A {\n"
        "  public C() { return; }\n"
        "  public int32 oops() { return super<Unrelated>.fn(); }\n"  // Unrelated is not a parent of C
        "}\n"
        "public final class D {\n"
        "  public static int32 run() {\n"
        "    C c = heap C();\n"
        "    return c.oops();\n"
        "  }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.D");
        FAIL() << "expected CAJETA_ERROR_NOT_AN_ANCESTOR";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_NOT_AN_ANCESTOR");
    }
}

TEST(MultiClassingPhase2Tests, thisBaseOnNonAncestorRejected) {
    auto src =
        "package test;\n"
        "public class A { public int32 x; public A() { return; } }\n"
        "public class Unrelated { public int32 x; public Unrelated() { return; } }\n"
        "public class C extends A {\n"
        "  public C() {\n"
        "    this<Unrelated>.x = 5;\n"  // Unrelated is not a parent of C
        "  }\n"
        "}\n"
        "public final class D {\n"
        "  public static int32 run() {\n"
        "    C c = heap C();\n"
        "    return 0;\n"
        "  }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.D");
        FAIL() << "expected CAJETA_ERROR_NOT_AN_ANCESTOR";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_NOT_AN_ANCESTOR");
    }
}
