//
// MultiClassing R-2 — narrow implicit-ctor-skip warning.
//
// Design: cajeta-docs/stdlib/MultiClassing.md § R-2.
//
// Cajeta's implicit super-ctor loop walks every declared parent of
// a subclass at construction time and calls each parent's no-arg
// ctor (when one exists). Today's grammar lets the user write
// `super(args)` to explicitly pick the FIRST parent's args ctor;
// the implicit loop still picks the no-arg for every OTHER parent.
//
// R-2's footgun: a sibling (non-first) parent has BOTH a no-arg
// AND an args ctor. The user wrote explicit `super(args)` for the
// first parent and may not have realized the sibling's args ctor
// got skipped. Warn so they can pick the sibling's args ctor too
// (via `super<Sibling>(args)` once that grammar lands, or via
// composition / restructured ctors today).
//
// The warning is narrow on purpose — emitting on every implicit
// no-arg pickup would flood the build log with noise on intentional
// skips. Three conditions must all hold:
//   1. User wrote an explicit super(args) in the ctor body.
//   2. A sibling (non-first) parent has BOTH a no-arg AND an
//      args ctor.
//   3. The no-arg got picked implicitly (always true today for
//      siblings — the implicit loop has no way to know which
//      ctor flavor the user wanted).
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

std::string compileCaptureStderr(const std::string& src) {
    testing::internal::CaptureStderr();
    try {
        CajetaJit::compile(src, "test.D");
    } catch (...) {
        // Some test shapes may compile-fail; capture what stderr
        // has anyway.
    }
    return testing::internal::GetCapturedStderr();
}

} // namespace

// --- Positive: warning fires for the footgun shape ----------------------

TEST(ImplicitCtorSkipWarningTests, warnsWhenSiblingHasBothCtorFlavors) {
    auto src =
        "package test;\n"
        "public class A {\n"
        "  public int32 ax;\n"
        "  public A() { this.ax = 0; }\n"
        "  public A(int32 v) { this.ax = v; }\n"
        "}\n"
        "public class B {\n"
        "  public int32 bx;\n"
        "  public B() { this.bx = 0; }\n"        // sibling has no-arg
        "  public B(int32 v) { this.bx = v; }\n"  // AND args
        "}\n"
        "public class C extends A, B {\n"
        "  public C() { super(7); }\n"  // explicit picks A(7); B's no-arg picked implicitly
        "}\n"
        "public final class D {\n"
        "  public static int32 run() { return 0; }\n"
        "}\n";
    std::string err = compileCaptureStderr(src);
    EXPECT_NE(err.find("[implicit-ctor-skip]"), std::string::npos)
        << "expected [implicit-ctor-skip] warning, got: " << err;
    // Should name the sibling parent that has both ctor flavors.
    EXPECT_NE(err.find("test.B"), std::string::npos)
        << "expected warning to name sibling 'test.B', got: " << err;
}

// --- Negative: no warning when sibling has only a no-arg ctor -----------

TEST(ImplicitCtorSkipWarningTests, noWarnWhenSiblingHasOnlyNoArg) {
    auto src =
        "package test;\n"
        "public class A {\n"
        "  public A() { return; }\n"
        "  public A(int32 v) { return; }\n"
        "}\n"
        "public class B {\n"
        "  public B() { return; }\n"  // only no-arg, no args ctor
        "}\n"
        "public class C extends A, B {\n"
        "  public C() { super(7); }\n"
        "}\n"
        "public final class D {\n"
        "  public static int32 run() { return 0; }\n"
        "}\n";
    std::string err = compileCaptureStderr(src);
    EXPECT_EQ(err.find("[implicit-ctor-skip]"), std::string::npos)
        << "expected no [implicit-ctor-skip] warning (sibling has only no-arg), got: " << err;
}

// --- Negative: no warning when user didn't write explicit super(args) ---

TEST(ImplicitCtorSkipWarningTests, noWarnWithoutExplicitSuper) {
    auto src =
        "package test;\n"
        "public class A {\n"
        "  public A() { return; }\n"
        "  public A(int32 v) { return; }\n"
        "}\n"
        "public class B {\n"
        "  public B() { return; }\n"
        "  public B(int32 v) { return; }\n"  // both flavors
        "}\n"
        "public class C extends A, B {\n"
        "  public C() { return; }\n"  // NO explicit super-call
        "}\n"
        "public final class D {\n"
        "  public static int32 run() { return 0; }\n"
        "}\n";
    std::string err = compileCaptureStderr(src);
    EXPECT_EQ(err.find("[implicit-ctor-skip]"), std::string::npos)
        << "expected no [implicit-ctor-skip] warning (no explicit super), got: " << err;
}

// --- Negative: no warning when sibling has only args ctor ----------------
//
// If sibling has no no-arg, the implicit loop skips it entirely
// (existing behavior — no double-init). No warning fires because
// no implicit pickup happened.

TEST(ImplicitCtorSkipWarningTests, noWarnWhenSiblingHasOnlyArgs) {
    auto src =
        "package test;\n"
        "public class A {\n"
        "  public A() { return; }\n"
        "  public A(int32 v) { return; }\n"
        "}\n"
        "public class B {\n"
        "  public B(int32 v) { return; }\n"  // only args, no no-arg
        "}\n"
        "public class C extends A, B {\n"
        "  public C() { super(7); }\n"  // explicit super(args) for A
        "}\n"
        "public final class D {\n"
        "  public static int32 run() { return 0; }\n"
        "}\n";
    std::string err = compileCaptureStderr(src);
    EXPECT_EQ(err.find("[implicit-ctor-skip]"), std::string::npos)
        << "expected no [implicit-ctor-skip] warning (sibling has only args), got: " << err;
}
