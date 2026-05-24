//
// adjustForUpcast threading tests (L-03 polymorphism follow-up).
//
// `CajetaClass::adjustForUpcast` is wired at `LocalVariableDeclaration`
// (Phase 1 of poly-MI) — `B b = new C()` correctly shifts the pointer
// to B's sub-object inside C's layout, so a later `b.method()`
// dispatches through B's secondary vtable in C.
//
// This test file covers the OTHER three upcast sites that need the
// same shift: assignment expressions (`b = c`), return statements
// (`return c` where the method's declared return type is B), and
// parameter passing (`foo(c)` where the parameter is B-typed). Each
// case uses a NON-FIRST parent path so the bug actually manifests
// (the first parent's sub-object always shares offset 0 — no shift
// needed).
//
// Test shape: `C extends A, B` where both A and B declare a field
// + an inherited method. The non-first parent (B) sub-object is
// offset > 0 in C's layout. A read of B's field via a B-typed
// binding fails today (raw C pointer, GEPed via B's struct type,
// lands on A's content). After the threading lands, the binding
// holds B's adjusted pointer and the GEP lands correctly.
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

// --- Assignment upcast ---------------------------------------------------
//
// `b = c` where b is B-typed and c is C-typed (C extends A, B). The
// RHS is a raw C pointer (offset 0 in C's layout); without the
// adjustment, b holds that raw pointer and `b.bx` GEPs B-struct-slot
// 1 from there → lands on A's content (wrong). After the fix, b
// holds the B-adjusted pointer and the GEP lands correctly.

TEST(UpcastThreadingTests, assignmentToNonFirstParentTypedSlotAdjustsPointer) {
    auto src =
        "package test;\n"
        "public class A { public int32 ax; public A() { return; } }\n"
        "public class B {\n"
        "  public int32 bx;\n"
        "  public B() { return; }\n"
        "  public int32 readBx() { return this.bx; }\n"
        "}\n"
        "public class C extends A, B {\n"
        "  public C() { this<A>.ax = 1; this<B>.bx = 42; }\n"
        "}\n"
        "public final class D {\n"
        "  public static int32 run() {\n"
        "    C c = new C();\n"
        "    B b;\n"
        "    b = c;\n"  // assignment to B-typed slot — needs adjustment
        "    return b.readBx();\n"  // 42 if b's pointer was adjusted to B's sub-object
        "  }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// --- Return upcast -------------------------------------------------------
//
// A method declared to return B but `return c;` (where c is C). The
// returned pointer must be B-adjusted so the caller's B-typed binding
// can dispatch correctly. Without the adjustment, the caller's
// b.bx access GEPs B's struct slot 1 from the raw C pointer → wrong.

TEST(UpcastThreadingTests, returnUpcastToNonFirstParentAdjustsPointer) {
    auto src =
        "package test;\n"
        "public class A { public int32 ax; public A() { return; } }\n"
        "public class B {\n"
        "  public int32 bx;\n"
        "  public B() { return; }\n"
        "  public int32 readBx() { return this.bx; }\n"
        "}\n"
        "public class C extends A, B {\n"
        "  public C() { this<A>.ax = 1; this<B>.bx = 73; }\n"
        "}\n"
        "public class Maker {\n"
        "  public Maker() { return; }\n"
        "  public #B makeAsB() {\n"
        "    C c = new C();\n"
        "    return c;\n"  // return C as B — needs adjustment
        "  }\n"
        "}\n"
        "public final class D {\n"
        "  public static int32 run() {\n"
        "    Maker m = new Maker();\n"
        "    B b = m.makeAsB();\n"  // b receives B-adjusted pointer
        "    return b.readBx();\n"  // 73
        "  }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 73);
}

// --- Parameter-passing upcast --------------------------------------------
//
// `foo(c)` where foo's parameter is B-typed. The arg passed to foo
// must be B-adjusted so the callee's `this`-like access works.
// Without the adjustment, foo sees the raw C pointer typed as B,
// and `param.bx` GEPs B's slot 1 from C → wrong.

TEST(UpcastThreadingTests, parameterPassingToNonFirstParentAdjustsPointer) {
    auto src =
        "package test;\n"
        "public class A { public int32 ax; public A() { return; } }\n"
        "public class B {\n"
        "  public int32 bx;\n"
        "  public B() { return; }\n"
        "}\n"
        "public class C extends A, B {\n"
        "  public C() { this<A>.ax = 1; this<B>.bx = 88; }\n"
        "}\n"
        "public class Reader {\n"
        "  public Reader() { return; }\n"
        "  public int32 readFromB(B b) { return b.bx; }\n"
        "}\n"
        "public final class D {\n"
        "  public static int32 run() {\n"
        "    C c = new C();\n"
        "    Reader r = new Reader();\n"
        "    return r.readFromB(c);\n"  // 88 if c is adjusted to B at the call site
        "  }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 88);
}

// --- Sanity: first-parent assignments still work -------------------------
//
// `A a = c` (where A is first parent) needs no adjustment (offset 0).
// Sanity check that the threading doesn't break the no-op case.

TEST(UpcastThreadingTests, assignmentToFirstParentNoShiftStillWorks) {
    auto src =
        "package test;\n"
        "public class A {\n"
        "  public int32 ax;\n"
        "  public A() { return; }\n"
        "  public int32 readAx() { return this.ax; }\n"
        "}\n"
        "public class B { public int32 bx; public B() { return; } }\n"
        "public class C extends A, B {\n"
        "  public C() { this<A>.ax = 17; this<B>.bx = 2; }\n"
        "}\n"
        "public final class D {\n"
        "  public static int32 run() {\n"
        "    C c = new C();\n"
        "    A a;\n"
        "    a = c;\n"  // first parent — offset 0, no adjust needed
        "    return a.readAx();\n"  // 17
        "  }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 17);
}

// --- Sanity: same-class assignment doesn't trigger adjustment ------------

TEST(UpcastThreadingTests, sameClassAssignmentDoesNotAdjust) {
    auto src =
        "package test;\n"
        "public class A {\n"
        "  public int32 x;\n"
        "  public A() { return; }\n"
        "}\n"
        "public final class D {\n"
        "  public static int32 run() {\n"
        "    A a1 = new A();\n"
        "    a1.x = 5;\n"
        "    A a2;\n"
        "    a2 = a1;\n"  // same class — no adjustment
        "    return a2.x;\n"  // 5
        "  }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 5);
}
