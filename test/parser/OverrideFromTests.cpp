//
// MultiClassing R-3 — `@Override(from=Parent)` optional annotation.
//
// Design: docs/specification/lang/MultiClassing.md § R-3.
//
// `@Override(from=B)` is accepted on any override; compiler verifies
// the named class is an ancestor of the receiver class AND declares
// a same-suffix method. Omitting `from=` keeps today's no-op
// behavior. Both `from=B` (identifier) and `from=B.class` (class
// literal) parse — the visitor classifies them as String / ClassRef
// respectively, so the verifier accepts either kind.
//
// Two error shapes are folded into one error id
// (CAJETA_ERROR_OVERRIDE_FROM_MISMATCH):
//   - The named class isn't an ancestor of the current class.
//   - The named ancestor doesn't declare a matching-suffix method
//     (typo on method name, or signature drift between parent and
//     child).
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

// --- Verified-correct uses --------------------------------------------------

TEST(OverrideFromTests, fromMatchingAncestorMethodCompiles) {
    auto src =
        "package test;\n"
        "public class A {\n"
        "  public A() { return; }\n"
        "  public int32 kind() { return 1; }\n"
        "}\n"
        "public class B extends A {\n"
        "  public B() { return; }\n"
        "  @Override(from=A) public int32 kind() { return 99; }\n"
        "}\n"
        "public final class D {\n"
        "  public static int32 run() {\n"
        "    B b = heap B();\n"
        "    return b.kind();\n"  // 99 — override + matching from=A
        "  }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 99);
}

TEST(OverrideFromTests, fromMatchingAncestorWithDotClassCompiles) {
    auto src =
        "package test;\n"
        "public class A {\n"
        "  public A() { return; }\n"
        "  public int32 kind() { return 7; }\n"
        "}\n"
        "public class B extends A {\n"
        "  public B() { return; }\n"
        "  @Override(from=A.class) public int32 kind() { return 11; }\n"
        "}\n"
        "public final class D {\n"
        "  public static int32 run() {\n"
        "    B b = heap B();\n"
        "    return b.kind();\n"  // 11
        "  }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 11);
}

// Multi-inheritance scenario where the user wants reader-cue
// confirmation that B::kind() is overriding the version inherited
// from A specifically (and not from the other sibling parent).

TEST(OverrideFromTests, fromMatchingAncestorAcrossMultipleInheritance) {
    auto src =
        "package test;\n"
        "public class A {\n"
        "  public A() { return; }\n"
        "  public int32 stride() { return 4; }\n"
        "}\n"
        "public class B {\n"
        "  public B() { return; }\n"
        "  public int32 stride() { return 5; }\n"
        "}\n"
        "public class C extends A, B {\n"
        "  public C() { return; }\n"
        "  @Override(from=B) public int32 stride() { return super<B>.stride() * 10; }\n"
        "}\n"
        "public final class D {\n"
        "  public static int32 run() {\n"
        "    C c = heap C();\n"
        "    return c.stride();\n"  // 5 * 10 = 50
        "  }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 50);
}

// --- Error cases ------------------------------------------------------------

TEST(OverrideFromTests, fromUnrelatedClassRejected) {
    auto src =
        "package test;\n"
        "public class A {\n"
        "  public A() { return; }\n"
        "  public int32 kind() { return 1; }\n"
        "}\n"
        "public class Unrelated {\n"
        "  public Unrelated() { return; }\n"
        "  public int32 kind() { return 1; }\n"
        "}\n"
        "public class B extends A {\n"
        "  public B() { return; }\n"
        "  @Override(from=Unrelated) public int32 kind() { return 99; }\n"  // Unrelated isn't B's ancestor
        "}\n"
        "public final class D {\n"
        "  public static int32 run() {\n"
        "    B b = heap B();\n"
        "    return b.kind();\n"
        "  }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.D");
        FAIL() << "expected CAJETA_ERROR_OVERRIDE_FROM_MISMATCH";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_OVERRIDE_FROM_MISMATCH");
    }
}

TEST(OverrideFromTests, fromAncestorWithoutMatchingMethodRejected) {
    auto src =
        "package test;\n"
        "public class A {\n"
        "  public A() { return; }\n"
        "  public int32 kind() { return 1; }\n"
        "}\n"
        "public class B extends A {\n"
        "  public B() { return; }\n"
        "  @Override(from=A) public int32 stridee() { return 99; }\n"  // typo: A has kind, not stridee
        "}\n"
        "public final class D {\n"
        "  public static int32 run() {\n"
        "    B b = heap B();\n"
        "    return b.kind();\n"
        "  }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.D");
        FAIL() << "expected CAJETA_ERROR_OVERRIDE_FROM_MISMATCH";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_OVERRIDE_FROM_MISMATCH");
    }
}

// --- Sanity: bare @Override still works (no-op verification) ---------------

TEST(OverrideFromTests, bareOverrideWithoutFromStillCompiles) {
    auto src =
        "package test;\n"
        "public class A {\n"
        "  public A() { return; }\n"
        "  public int32 kind() { return 1; }\n"
        "}\n"
        "public class B extends A {\n"
        "  public B() { return; }\n"
        "  @Override public int32 kind() { return 99; }\n"  // no from= — no verification
        "}\n"
        "public final class D {\n"
        "  public static int32 run() {\n"
        "    B b = heap B();\n"
        "    return b.kind();\n"  // 99
        "  }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 99);
}
