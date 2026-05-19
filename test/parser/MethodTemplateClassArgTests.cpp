// Parser disambiguation for method-template explicit type args with
// class-typed arguments: `expr.method<ClassName>(arg)` shapes.
//
// The grammar's `expression bop='.'` production lists alternatives in
// order, and ANTLR's tie-break picks the first alternative when both
// can complete the overall parse. Before 2026-05-19, `identifier`
// preceded `methodCall`, so for `Util.zap<Foo>(f)` ANTLR took the
// identifier path — consuming just `zap` and letting the outer
// expression rule absorb `<Foo>(f)` as a chained `<` / `>` comparison
// — producing a broken comparison AST that segfaulted at codegen.
// Swapping the order to put `methodCall` first makes ANTLR prefer
// the call interpretation and falls through to `identifier` only when
// methodCall fails to match (no `(` follows the identifier).
//
// These tests pin all four corner cases:
//   - single class-typed arg (the originally-broken shape)
//   - two args (comma blocked the comparison parse anyway)
//   - zero args (empty parens blocked it)
//   - primitive type arg with single value arg (some other ANTLR
//     internal kept this working)
// All four must pass to demonstrate the disambiguation is robust.

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"
#include <cstdint>

using cajeta_test::CajetaJit;

namespace {
int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}
} // namespace

// Originally-failing shape: static method, class type arg, ONE value arg.
TEST(MethodTemplateClassArgTests, staticClassArgSingle) {
    auto src =
        "package test;\n"
        "public class Foo { public int32 v; }\n"
        "public class Util {\n"
        "    public static <T> int32 zap(T value) { return 42; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Foo f = heap Foo();\n"
        "        return Util.zap<Foo>(f);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// Two args — comma blocks the comparison-chain parse.
TEST(MethodTemplateClassArgTests, staticClassArgTwo) {
    auto src =
        "package test;\n"
        "public class Foo { public int32 v; }\n"
        "public class Util {\n"
        "    public static <T> int32 zap(T value, int32 extra) { return 42; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Foo f = heap Foo();\n"
        "        return Util.zap<Foo>(f, 1);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// Zero args — empty parens block the parse.
TEST(MethodTemplateClassArgTests, staticClassArgZero) {
    auto src =
        "package test;\n"
        "public class Foo { public int32 v; }\n"
        "public class Util {\n"
        "    public static <T> int32 zap() { return 42; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        return Util.zap<Foo>();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// Primitive type arg with single value arg — control case.
TEST(MethodTemplateClassArgTests, staticPrimArgSingle) {
    auto src =
        "package test;\n"
        "public class Util {\n"
        "    public static <T> int32 zap(T value) { return 42; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        return Util.zap<int32>(7);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}
