//
// Tests for cajeta.lang.Stream<T> and ArrayStream<T> — the pull-protocol
// base for stdlib iteration (Collections.md § Stream, P6.4 of the
// unified-class rollout).
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

TEST(StreamTests, arrayStreamYieldsElementsThenNone) {
    // Three elements then exhausted. Sums the first two values, returns
    // a sentinel from the None branch — if next() incorrectly returns
    // garbage after exhaustion, the result would not be 30.
    auto src =
        "package test;\n"
        "import cajeta.lang.Optional;\n"
        "import cajeta.lang.ArrayStream;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = {10, 20, 30};\n"
        "        ArrayStream<int32> s = heap ArrayStream<int32>(xs, 3);\n"
        "        Optional<int32> a = s.next();\n"
        "        Optional<int32> b = s.next();\n"
        "        Optional<int32> c = s.next();\n"
        "        Optional<int32> d = s.next();\n"
        "        if (!d.isPresent()) { return a.get() + b.get() + c.get(); }\n"
        "        return -1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 60);
}

TEST(StreamTests, arrayStreamEmptyArray) {
    // Empty backing array → first next() returns None.
    auto src =
        "package test;\n"
        "import cajeta.lang.Optional;\n"
        "import cajeta.lang.ArrayStream;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = new int32[0];\n"
        "        ArrayStream<int32> s = heap ArrayStream<int32>(xs, 0);\n"
        "        Optional<int32> first = s.next();\n"
        "        if (first.isEmpty()) { return 1; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

TEST(StreamTests, streamBaseDefaultIsEmpty) {
    // Bare Stream<T> (not ArrayStream) — default next() returns empty.
    auto src =
        "package test;\n"
        "import cajeta.lang.Optional;\n"
        "import cajeta.lang.Stream;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Stream<int32> s = heap Stream<int32>();\n"
        "        Optional<int32> o = s.next();\n"
        "        if (o.isEmpty()) { return 1; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}
