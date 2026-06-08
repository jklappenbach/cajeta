//
// MultiClassing Phase 1 — collision rejection (P-1).
//
// Tests for the three new error codes raised at class-build / first-
// access time when a multi-class child has unresolved name collisions
// among its parents:
//
//   - CAJETA_ERROR_AMBIGUOUS_METHOD_DISPATCH
//   - CAJETA_ERROR_AMBIGUOUS_FIELD_ACCESS
//   - CAJETA_ERROR_RETURN_TYPE_COLLISION
//
// Design: docs/stdlib/MultiClassing.md § P-1 + § Phase 1.
//
// Each negative test pairs with a positive companion that exercises
// the same code paths after the user follows the remediation
// described in the error message (override in the child, or — once
// MultiClassing Phase 2 lands — `c[A].member` / `super<A>.method()`).
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

// --- Method ambiguity ------------------------------------------------------
//
// A and B independently define `kind()` with different concrete
// bodies. `C extends A, B` doesn't override, so a call like
// `c.kind()` has no single most-derived choice — today the alias
// walk silently picks the last-walked parent (B). Per P-1, this
// must fail at vtable build time so the user is forced to pick.

TEST(MultiClassingPhase1Tests, ambiguousMethodDispatchRejected) {
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
        "}\n"
        "public final class D {\n"
        "  public static int32 run() {\n"
        "    C c = heap C();\n"
        "    return c.kind();\n"
        "  }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.D");
        FAIL() << "expected CAJETA_ERROR_AMBIGUOUS_METHOD_DISPATCH";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_AMBIGUOUS_METHOD_DISPATCH");
    }
}

// Positive companion: C provides its own override → the ambiguity
// dissolves and the override is the canonical impl. (Once Phase 2
// lands, the override body can call `super<A>.kind() + super<B>.kind()`
// to combine; for Phase 1 just returning a literal exercises the
// resolution path.)

TEST(MultiClassingPhase1Tests, ambiguousMethodOverrideInChildResolves) {
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
        "  public int32 kind() { return 99; }\n"
        "}\n"
        "public final class D {\n"
        "  public static int32 run() {\n"
        "    C c = heap C();\n"
        "    return c.kind();\n"  // 99 — C's override wins
        "  }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 99);
}

// Disjoint methods (no name overlap) across two parents must keep
// compiling cleanly — the ambiguity detection must not over-fire.

TEST(MultiClassingPhase1Tests, disjointMethodsAcrossParentsCompile) {
    auto src =
        "package test;\n"
        "public class A {\n"
        "  public A() { return; }\n"
        "  public int32 foo() { return 7; }\n"
        "}\n"
        "public class B {\n"
        "  public B() { return; }\n"
        "  public int32 bar() { return 11; }\n"
        "}\n"
        "public class C extends A, B {\n"
        "  public C() { return; }\n"
        "}\n"
        "public final class D {\n"
        "  public static int32 run() {\n"
        "    C c = heap C();\n"
        "    return c.foo() + c.bar();\n"  // 18
        "  }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 18);
}

// P-5 doc rule: an abstract method in one parent and a concrete
// method with the same suffix in another parent is NOT a conflict —
// the concrete one satisfies the obligation. The abstract-obligation
// check in `buildVirtualTable` already accepts the class shape;
// dispatch through the abstract canonical now also lands on the
// concrete impl after the alias walk includes abstract-declared
// canonicals (their suffix is matched against bySuffix and aliased
// to the most-derived concrete impl).
TEST(MultiClassingPhase1Tests, abstractInOneParentConcreteInOtherSatisfies) {
    auto src =
        "package test;\n"
        "public abstract class A {\n"
        "  public A() { return; }\n"
        "  public abstract int32 step();\n"
        "}\n"
        "public class B {\n"
        "  public B() { return; }\n"
        "  public int32 step() { return 42; }\n"
        "}\n"
        "public class C extends A, B {\n"
        "  public C() { return; }\n"
        "}\n"
        "public final class D {\n"
        "  public static int32 run() {\n"
        "    C c = heap C();\n"
        "    return c.step();\n"  // 42 — B's concrete satisfies A's obligation
        "  }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// Common-ancestor methods must NOT trip the ambiguity check. X
// declares `m()`; A and B both extend X; D extends A, B. Both paths
// to X.m lead to the SAME method declaration — there's only one
// impl, just two paths reaching it. This is a true diamond
// (P-4 § Scenario 1) and resolves cleanly without an override.

TEST(MultiClassingPhase1Tests, commonAncestorMethodDoesNotConflict) {
    auto src =
        "package test;\n"
        "public class X {\n"
        "  public X() { return; }\n"
        "  public int32 m() { return 5; }\n"
        "}\n"
        "public class A extends X {\n"
        "  public A() { return; }\n"
        "}\n"
        "public class B extends X {\n"
        "  public B() { return; }\n"
        "}\n"
        "public class Z extends A, B {\n"
        "  public Z() { return; }\n"
        "}\n"
        "public final class D {\n"
        "  public static int32 run() {\n"
        "    Z z = heap Z();\n"
        "    return z.m();\n"  // 5 — single impl, single answer
        "  }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 5);
}

// --- Field ambiguity -------------------------------------------------------
//
// A and B independently declare `total`. Today `c.total` would
// resolve to whichever the DFS field-walk hits first, silently
// picking one slot and ignoring the other. Per P-1, naked `c.total`
// is ambiguous and must error; `c[A].total` and `c[B].total` (after
// Phase 2) reach the independent storage.

TEST(MultiClassingPhase1Tests, ambiguousFieldAccessRejected) {
    auto src =
        "package test;\n"
        "public class A { public int32 total; public A() { return; } }\n"
        "public class B { public int32 total; public B() { return; } }\n"
        "public class C extends A, B {\n"
        "  public C() { return; }\n"
        "}\n"
        "public final class D {\n"
        "  public static int32 run() {\n"
        "    C c = heap C();\n"
        "    return c.total;\n"
        "  }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.D");
        FAIL() << "expected CAJETA_ERROR_AMBIGUOUS_FIELD_ACCESS";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_AMBIGUOUS_FIELD_ACCESS");
    }
}

// Disjoint fields across parents must keep compiling — sanity that
// the field-name ambiguity check doesn't over-fire on unrelated names.

TEST(MultiClassingPhase1Tests, disjointFieldsAcrossParentsCompile) {
    auto src =
        "package test;\n"
        "public class A { public int32 a; public A() { this.a = 3; } }\n"
        "public class B { public int32 b; public B() { this.b = 4; } }\n"
        "public class C extends A, B {\n"
        "  public C() { return; }\n"
        "}\n"
        "public final class D {\n"
        "  public static int32 run() {\n"
        "    C c = heap C();\n"
        "    return c.a + c.b;\n"  // 7
        "  }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

// --- Return-type collision -------------------------------------------------
//
// `Method::toCanonical` does NOT include the return type (only
// name + params), so `void fn()` and `int32 fn()` have the same
// suffix. Today this collapses to one vtable slot and the runtime
// indirect-call casts the result of one method through the other's
// signature — garbage values, no warning. Must error at vtable
// build with CAJETA_ERROR_RETURN_TYPE_COLLISION.

TEST(MultiClassingPhase1Tests, returnTypeCollisionRejected) {
    auto src =
        "package test;\n"
        "public class A {\n"
        "  public A() { return; }\n"
        "  public void fn() { return; }\n"
        "}\n"
        "public class B {\n"
        "  public B() { return; }\n"
        "  public int32 fn() { return 7; }\n"
        "}\n"
        "public class C extends A, B {\n"
        "  public C() { return; }\n"
        "}\n"
        "public final class D {\n"
        "  public static int32 run() {\n"
        "    C c = heap C();\n"
        "    return 0;\n"
        "  }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.D");
        FAIL() << "expected CAJETA_ERROR_RETURN_TYPE_COLLISION";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_RETURN_TYPE_COLLISION");
    }
}
