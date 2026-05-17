//
// Inline-intrinsic (and inline-call-expr) ctor argument forms.
// `heap T(xs.stream(), n)` — the result of one method call gets passed
// directly into another constructor without an intermediate local.
// Crashed historically: the inner call's resolveTypes/generateCode
// ordering mismatched the outer ctor's parameter coercion path.
//
// Coverage:
//   - Inline `.stream()` intrinsic as ctor arg
//   - Inline user-method call as ctor arg
//   - Inline `heap X(...)` as ctor arg (nested heap)
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

TEST(InlineCtorArgTests, dotStreamIntrinsicAsCtorArg) {
    auto src =
        "package test;\n"
        "import cajeta.lang.TakeStream;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = {1, 2, 3, 4, 5};\n"
        "        TakeStream<int32> t = heap TakeStream<int32>(xs.stream(), 4);\n"
        "        return t.count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 4);
}

TEST(InlineCtorArgTests, nestedHeapInCtorArg) {
    auto src =
        "package test;\n"
        "import cajeta.lang.ArrayStream;\n"
        "import cajeta.lang.TakeStream;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = {1, 2, 3, 4, 5};\n"
        "        TakeStream<int32> t = heap TakeStream<int32>(\n"
        "            heap ArrayStream<int32>(xs, 5), 3);\n"
        "        return t.count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 3);
}

TEST(InlineCtorArgTests, userMethodResultAsCtorArg) {
    // No stdlib intrinsic — a user-defined static method returning a
    // class-typed value is passed inline. Pins the more general
    // "inline method call as ctor arg" behavior; if this works but
    // the .stream() form doesn't, the bug is intrinsic-specific.
    auto src =
        "package test;\n"
        "import cajeta.lang.ArrayStream;\n"
        "import cajeta.lang.TakeStream;\n"
        "public final class Mk {\n"
        "    public static ArrayStream<int32> make(int32[] xs) {\n"
        "        return heap ArrayStream<int32>(xs, 5);\n"
        "    }\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = {1, 2, 3, 4, 5};\n"
        "        TakeStream<int32> t = heap TakeStream<int32>(Mk.make(xs), 2);\n"
        "        return t.count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 2);
}
