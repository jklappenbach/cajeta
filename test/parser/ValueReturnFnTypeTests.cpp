//
// M5(b) — function-pointer / method-reference types carrying sret. With the
// value-return ABI now visible through CajetaFunctionType (see
// cajeta-docs/stdlib/ValueReturns.md), a lambda whose body is a `stack X(...)`
// construction lowers to a sret-shaped function, and a method reference to a
// value-returning method (one that returnsStackValue() per the body scan)
// produces a sret-shaped function-type. The call site allocates the result
// slot in its own frame and threads it as the closure's hidden arg 0.
//
// Source-level discrimination: `(P) -> R` is the sret value-return form;
// `(P) -> #R` is the ownership / heap-return form. The grammar's
// `typeTypeOrVoid REFERENCE?` token threads `returnsOwn` through
// CajetaType::fromContext and LambdaExpression::resolveTypes (mirrored from
// Method::returnsStackValue body scans where the type is inferred).
//
// Tests below exercise the full M5(b) matrix:
//   - primitiveFnTypedLocalSmoke           — shared closure ABI regression guard
//   - lambdaStackReturnIntoSretCallback    — lambda `stack X(...)` direct (N3)
//   - lambdaBlockStackReturnIntoSretCallback — block-body variant
//   - methodRefToSretMethodDirectBinding   — method-ref sret→sret direct (N4)
//   - ownershipFormConstructorRefStillWorks — `#R` path lock-in
//   - methodRefBorrowToSretAdapter         — borrow→sret synthesized thunk (N5)
//   - methodRefOwnershipToSretRejected     — matrix `#R → R` error
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

// Smoke: explicit fn-typed local with primitive return invoked through the
// indirect-call site. Confirms the shared closure ABI (the path the sret form
// extends) still works end-to-end.
TEST(ValueReturnFnTypeTests, primitiveFnTypedLocalSmoke) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        (int32) -> int32 f = (int32 x) -> x + 1;\n"
        "        return f(41);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// Lambda body is `stack X(...)`; the LHS sret-form fn-type pins the ABI
// (returnsOwn=false). The lambda's underlying function takes the sret slot
// at arg 0; NRVO constructs the Optional directly into the caller-owned
// slot the indirect-call site allocates. M5(b) — matched-ABI direct path.
TEST(ValueReturnFnTypeTests, lambdaStackReturnIntoSretCallback) {
    auto src =
        "package test;\n"
        "import cajeta.lang.Optional;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        (int32) -> Optional<int32> f = (int32 x) -> stack Optional<int32>(true, x);\n"
        "        Optional<int32> o = f(42);\n"
        "        return o.get();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// Block-body variant — Method::nodeHasStackReturn (now exposed for lambdas)
// walks the block for any `return stack X(...)`. The signature-driven
// ReturnStatement path in Statement.cpp fires the NRVO/memcpy into the
// lambda's sret slot.
TEST(ValueReturnFnTypeTests, lambdaBlockStackReturnIntoSretCallback) {
    auto src =
        "package test;\n"
        "import cajeta.lang.Optional;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        (int32) -> Optional<int32> f = (int32 x) -> {\n"
        "            return stack Optional<int32>(true, x + 1);\n"
        "        };\n"
        "        Optional<int32> o = f(7);\n"
        "        return o.get();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 8);
}

// Method reference to a value-returning method, bound through an sret-form
// fn-typed local. resolveTypes computes returnsOwn=!returnsStackValue() so
// `m::mk` produces the same canonical the LHS declared; the thunk's
// underlying call forwards the sret slot straight to Maker.mk's sret arg.
TEST(ValueReturnFnTypeTests, methodRefToSretMethodDirectBinding) {
    auto src =
        "package test;\n"
        "import cajeta.lang.Optional;\n"
        "public class Maker {\n"
        "    public Optional<int32> mk() {\n"
        "        return stack Optional<int32>(true, 99);\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Maker m = heap Maker();\n"
        "        () -> Optional<int32> f = m::mk;\n"
        "        Optional<int32> o = f();\n"
        "        return o.get();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 99);
}

// Negative-shape control: an ownership-form (`#R`) fn-type still works as
// before (constructor refs, heap-returning lambdas). Locks in the matrix's
// "direct" diagonal for the heap-ownership path.
TEST(ValueReturnFnTypeTests, ownershipFormConstructorRefStillWorks) {
    auto src =
        "package test;\n"
        "public class Counter {\n"
        "    public int32 v;\n"
        "    public Counter() { this.v = 5; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        () -> #Counter ctor = Counter::new;\n"
        "        Counter c = ctor();\n"
        "        return c.v;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 5);
}

// M5(b) adapter — assigning a method reference to a borrow-returning
// method (`Box::peek` returns the owned inner Counter) into an sret-form
// callback. The thunk synthesizes a copy: call peek to get the borrowed
// R*, memcpy the bytes into the caller's sret slot, ret void. The
// caller's local now holds an independent struct-resident copy whose
// drop is the stack-drop variant.
TEST(ValueReturnFnTypeTests, methodRefBorrowToSretAdapter) {
    auto src =
        "package test;\n"
        "public class Counter {\n"
        "    public int32 v;\n"
        "    public Counter(int32 v) { this.v = v; }\n"
        "}\n"
        "public class Box {\n"
        "    Counter inner;\n"
        "    public Box(int32 v) { this.inner = heap Counter(v); }\n"
        "    public Counter peek() { return this.inner; }\n"  // borrow return
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Box b = heap Box(73);\n"
        "        () -> Counter f = b::peek;\n"  // sret form via expectedType override
        "        Counter c = f();\n"
        "        return c.v;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 73);
}

// Matrix rejection: ownership-returning method (`Maker::make`) into an
// sret slot. The plan's "would leak" error fires at resolveTypes — we
// never reach generateCode.
TEST(ValueReturnFnTypeTests, methodRefOwnershipToSretRejected) {
    auto src =
        "package test;\n"
        "public class Box {\n"
        "    public int32 v;\n"
        "    public Box(int32 v) { this.v = v; }\n"
        "}\n"
        "public class Maker {\n"
        "    public #Box make() { return heap Box(11); }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Maker m = heap Maker();\n"
        "        () -> Box f = m::make;\n"  // matrix error — # → sret rejected
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_THROW(runI32(src), cajeta::Exception);
}
