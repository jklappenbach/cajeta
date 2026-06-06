//
// S4 — view methods (Views.md § Methods).
//
// Views can declare plain methods. `this` is the view pointer; methods
// read/write fields, return primitives or owned values, call other
// methods on self. Restrictions: no virtual (views have no vtable),
// no methods returning borrows into `this` (defers nested-borrow
// tracking), no method-level generic type parameters.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

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

TEST(ViewMethodsTests, readOnlyMethodReturnsPrimitive) {
    auto src =
        "package test;\n"
        "@HostEndian\n"
        "public view Duo {\n"
        "    int32 a;\n"
        "    int32 b;\n"
        "    public int32 sum() {\n"
        "        return this.a + this.b;\n"
        "    }\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32[] bytes = heap int32[2];\n"
        "        Duo p = Duo(bytes);\n"
        "        p.a = 19;\n"
        "        p.b = 23;\n"
        "        return p.sum();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

TEST(ViewMethodsTests, writingMethodMutatesBuffer) {
    auto src =
        "package test;\n"
        "@HostEndian\n"
        "public view Counter {\n"
        "    int32 value;\n"
        "    public void bump() {\n"
        "        this.value = this.value + 1;\n"
        "    }\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32[] bytes = heap int32[1];\n"
        "        Counter c = Counter(bytes);\n"
        "        c.value = 40;\n"
        "        c.bump();\n"
        "        c.bump();\n"
        "        return c.value;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

TEST(ViewMethodsTests, methodCallingAnotherMethodOnSelf) {
    auto src =
        "package test;\n"
        "@HostEndian\n"
        "public view Calc {\n"
        "    int32 x;\n"
        "    public int32 doubled() {\n"
        "        return this.x + this.x;\n"
        "    }\n"
        "    public int32 quadrupled() {\n"
        "        return this.doubled() + this.doubled();\n"
        "    }\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32[] bytes = heap int32[1];\n"
        "        Calc c = Calc(bytes);\n"
        "        c.x = 10;\n"
        "        return c.quadrupled();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 40);
}

TEST(ViewMethodsTests, multipleReadsAndWritesThroughMethods) {
    // Coverage that methods can interleave reads and writes — exercises
    // the bswap-on-load + bswap-on-store paths through the method body.
    auto src =
        "package test;\n"
        "@BigEndian\n"
        "public view Tally {\n"
        "    int32 a;\n"
        "    int32 b;\n"
        "    int32 c;\n"
        "    public void addToA(int32 v) { this.a = this.a + v; }\n"
        "    public void addToB(int32 v) { this.b = this.b + v; }\n"
        "    public int32 finalize() { this.c = this.a + this.b; return this.c; }\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32[] bytes = heap int32[3];\n"
        "        Tally t = Tally(bytes);\n"
        "        t.a = 10;\n"
        "        t.b = 5;\n"
        "        t.addToA(7);\n"
        "        t.addToB(20);\n"
        "        return t.finalize();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

TEST(ViewMethodsTests, staticMethodOnView) {
    // Static methods on a view: no `this`, callable via View.methodName().
    // Useful for view-related utility helpers (factory-style validation,
    // canonical-value providers, etc.). Tests that the static dispatch
    // path also works on the new aggregate type, not just instance calls.
    auto src =
        "package test;\n"
        "@HostEndian\n"
        "public view Tag {\n"
        "    int32 value;\n"
        "    public static int32 sentinel() { return 1234567; }\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        return Tag.sentinel();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1234567);
}

// Deferred negative test, kept as a TODO marker so it doesn't silently
// disappear from the S4 plan:
//
//   - methodReturningSelfBorrowRejected: returning a borrow into
//     `this` (e.g. a String field of the view) should be rejected per
//     Views.md § Methods. Detection requires a data-flow pass that
//     traces return-value provenance back through field reads; deferred.
//
// Note on the S4 plan's "method-level generics rejected" item: that
// turned out to be a non-restriction. The grammar (CajetaParser.g4
// methodDeclaration) doesn't include typeParameters on methods at all
// — method-level generics aren't supported anywhere in cajeta, by
// design (see interfaceMemberDeclaration comment in the grammar).
// There's nothing view-specific to enforce. The parser does mishandle
// `<T>` written on a method (stderr error then segfault), but that's
// a general parser-error-handling issue, not part of S4 scope.
