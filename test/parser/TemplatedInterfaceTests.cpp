// Tests that templated interfaces parse and can be referenced.
// Pre-fix, adding `interface Foo<T> { ... }` to stdlib crashed the
// stdlib build at parse time because visitInterfaceDeclaration
// didn't process typeParameters — the body methods referenced
// unresolved T placeholders.
//
// v1 minimal: declaration parses; class can declare `implements
// Foo<X>`. Full instantiation + interface vtable for templated
// interfaces is future work — the @Encoding path that needs an
// Encoder<T> contract stays duck-typed for now.

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"

#include <cstdint>

using cajeta_test::CajetaJit;

// User-defined templated interface compiles.
TEST(TemplatedInterfaceTests, declarationCompiles) {
    auto src =
        "package test;\n"
        "public interface Codec<T> {\n"
        "    int32 encode(T v);\n"
        "    T decode(int32 raw);\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() { return 1; }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 1);
}

// Stdlib's cajeta.wire.Encoder<T> is reachable from user code.
TEST(TemplatedInterfaceTests, stdlibEncoderReachable) {
    auto src =
        "package test;\n"
        "import cajeta.wire.Encoder;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        // Just reference the type — no instantiation needed.
        "        return 1;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 1);
}
