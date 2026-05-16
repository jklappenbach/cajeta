//
// Session 8 — struct methods (direct calls only).
//
// Per StructsViewsStatus.md S8, a struct can declare methods just like a
// class. The `this` parameter is the struct pointer (aggregate-by-pointer
// per CajetaAggregate's calling convention). Direct calls inline at the
// call site; LLVM's inliner takes care of the rest given the static
// target. Field accesses through `this` GEP into the struct slot via the
// same DotExpression path that handles external `p.field` reads.
//
// Interface dispatch on struct methods is Session 9-11's concern.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.S");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

// ---------------------------------------------------------------------
// S8.1 — parser accepts method declarations on structs. structDeclaration
// shares classBody with classDeclaration, so methods already parsed
// syntactically. With S6 / S7 in place, codegen for the method body
// works too.
// ---------------------------------------------------------------------

// Simplest getter — single primitive field, return it. Confirms the
// method declaration parses and the resulting method is callable.
TEST(StructMethodsTests, simpleGetter) {
    auto src =
        "package test;\n"
        "public struct Holder {\n"
        "    int32 value;\n"
        "    public int32 get() { return this.value; }\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Holder h = Holder { value: 42 };\n"
        "        return h.get();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// ---------------------------------------------------------------------
// S8.2 — codegen: struct method = LLVM function with `this` as struct
// pointer. CajetaClass::invokeMethod recognizes aggregates and skips
// the vtable indirection (added in S4.2 for views); the call site
// passes the body alloca pointer directly, and the callee GEPs through
// `this` to reach fields. LLVM's inliner takes care of monomorphizing
// the call after layout.
// ---------------------------------------------------------------------

// Mutating method — writes to `this.field` must be visible after the
// call returns, because `this` is a pointer to the caller's body
// alloca, not a copy. If aggregates were pass-by-value here, the
// mutation would land in a callee-local copy and be invisible.
TEST(StructMethodsTests, mutatingMethodVisibleAfterCall) {
    auto src =
        "package test;\n"
        "public struct Counter {\n"
        "    int32 value;\n"
        "    public void bump(int32 by) {\n"
        "        this.value = this.value + by;\n"
        "    }\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Counter c = Counter { value: 10 };\n"
        "        c.bump(5);\n"
        "        c.bump(7);\n"
        "        return c.value;\n"  // 22
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 22);
}

// Method takes a parameter alongside `this`. The parameter sits at
// LLVM index 1 (after the implicit `this`); the cajeta call site
// supplies it positionally.
TEST(StructMethodsTests, methodWithExtraParameter) {
    auto src =
        "package test;\n"
        "public struct Holder {\n"
        "    int32 value;\n"
        "    public int32 plus(int32 n) {\n"
        "        return this.value + n;\n"
        "    }\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Holder h = Holder { value: 100 };\n"
        "        return h.plus(23);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 123);
}
