//
// Tests for cajeta.lang.Pair<K,V> — the stdlib's two-field generic
// value type (Collections.md § Pair, P6.1 of the unified-class rollout).
// Pair is the foundation for HashMap.entries(), Stream.zip(), and any
// future stdlib API that pairs two values.
//
// Generic instantiation tests sit alongside the storage-mode tests so
// future regressions surface at the right layer.
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

TEST(PairTests, heapPairWithIntKeyAndIntValue) {
    auto src =
        "package test;\n"
        "import cajeta.lang.Pair;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Pair<int32, int32> p = heap Pair<int32, int32>(3, 4);\n"
        "        return p.first() * p.first() + p.second() * p.second();\n"  // 9 + 16 = 25
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 25);
}

TEST(PairTests, stackPairWithIntKeyAndIntValue) {
    auto src =
        "package test;\n"
        "import cajeta.lang.Pair;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Pair<int32, int32> p = stack Pair<int32, int32>(7, 11);\n"
        "        return p.first() + p.second();\n"  // 18
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 18);
}

// Aggregate-init on generic types (`Pair<int32, int32> { first: 100, second: 200 }`)
// isn't supported in v1 — the aggregateInitializer grammar accepts an
// identifier but not type arguments. Use the constructor-call form
// (`Pair<int32, int32>(100, 200)`) until aggregate-init grammar extends.

TEST(PairTests, pairWithMixedPrimitiveTypes) {
    // Different K and V primitive types — confirms monomorphization
    // produces distinct concrete Pair<int32, int64> vs Pair<int32, int32>.
    auto src =
        "package test;\n"
        "import cajeta.lang.Pair;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Pair<int32, int64> p = heap Pair<int32, int64>(5, 100);\n"
        "        return p.first() + (int32) p.second();\n"  // 5 + 100
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 105);
}
