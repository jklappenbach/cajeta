//
// `Stream<int32> src = xs.stream();` style: assign an intrinsic-
// returned templated instance to a parent-typed local. Before the fix,
// this tripped a JIT linker error ("Symbols not found:
// ArrayStream<int32>::ArrayStream / ::next") because the .stream()
// intrinsic instantiates ArrayStream<int32> in the stdlib module
// MID-codegen of the user method — past the point where the codegen
// loop already swept stdlib's methods. The fix loops Phase 1 + Phase 2
// until quiescent so new instantiations land bodies in the same
// compilation.
//
// Pre-fix workaround was either an exact-typed local
// (`ArrayStream<int32> as = xs.stream();`) or an explicit two-step
// upcast (`ArrayStream<int32> as = …; Stream<int32> src = as;`).
// Post-fix the direct upcast form works.
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

TEST(UpcastInitializerTests, dotStreamAssignedToBaseStreamLocal) {
    auto src =
        "package test;\n"
        "import cajeta.lang.Stream;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = {1, 2, 3, 4, 5};\n"
        "        Stream<int32> src = xs.stream();\n"
        "        return src.count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 5);
}

TEST(UpcastInitializerTests, dotStreamAssignedToBaseStreamLocalThenIntoCtor) {
    // Combines the upcast with subtype-aware ctor lookup (the other
    // P1.1 sub-item) — direct chain through TakeStream.
    auto src =
        "package test;\n"
        "import cajeta.lang.Stream;\n"
        "import cajeta.lang.TakeStream;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = {1, 2, 3, 4, 5};\n"
        "        Stream<int32> src = xs.stream();\n"
        "        TakeStream<int32> t = heap TakeStream<int32>(src, 3);\n"
        "        return t.count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 3);
}

// Note: passing `xs.stream()` directly as a ctor arg without binding to
// a local first (`heap TakeStream<int32>(xs.stream(), 4)`) currently
// crashes — separate from the upcast/symbol-resolution fix and the
// subtype-aware ctor lookup. Tracked as a follow-up in ToDo.md.
