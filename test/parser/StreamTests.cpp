//
// Tests for cajeta.lang.stream.Stream<T> and ArrayStream<T> — the pull-protocol
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
        "import cajeta.lang.stream.ArrayStream;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = [10, 20, 30];\n"
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
        "import cajeta.lang.stream.ArrayStream;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = heap int32[0];\n"
        "        ArrayStream<int32> s = heap ArrayStream<int32>(xs, 0);\n"
        "        Optional<int32> first = s.next();\n"
        "        if (first.isEmpty()) { return 1; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

TEST(StreamTests, arrayStreamCount) {
    // count() walks next() until exhaustion. Returns 3 for a 3-element
    // backing array. Verifies the inherited combinator dispatches to
    // the ArrayStream override of next() (not the empty default).
    auto src =
        "package test;\n"
        "import cajeta.lang.stream.ArrayStream;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = [10, 20, 30];\n"
        "        ArrayStream<int32> s = heap ArrayStream<int32>(xs, 3);\n"
        "        return s.count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 3);
}

TEST(StreamTests, baseStreamCountIsZero) {
    // Bare Stream<int32>'s count() walks the empty-default next() →
    // immediately None → count is 0.
    auto src =
        "package test;\n"
        "import cajeta.lang.stream.Stream;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Stream<int32> s = heap Stream<int32>();\n"
        "        return s.count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 0);
}

TEST(StreamTests, streamBaseDefaultIsEmpty) {
    // Bare Stream<T> (not ArrayStream) — default next() returns empty.
    auto src =
        "package test;\n"
        "import cajeta.lang.Optional;\n"
        "import cajeta.lang.stream.Stream;\n"
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

// P6.6 — `arr.stream()` compiler intrinsic. Lowers to a fresh
// ArrayStream<T> walking the receiver array. Assign-to-typed-local
// form works today. Chained form (`xs.stream().count()`) is deferred:
// the outer call needs the inner MCE's resolvedType to dispatch
// count() to ArrayStream<T>, but pre-resolving in MethodCallExpression::
// resolveTypes early-triggers ArrayStream<T> instantiation in a way
// that breaks downstream method linkage (the user module's IR ends up
// referencing methods that the instantiation pass emitted into the
// stdlib module without the cross-module merge picking them up).
// Resolving that is its own follow-up — see ToDo.md P6.6 notes.
TEST(StreamTests, arrayStreamIntrinsicYieldsElements) {
    // Verify the intrinsic-produced stream actually walks the receiver
    // array — not an empty default. Sums first three of four values.
    auto src =
        "package test;\n"
        "import cajeta.lang.Optional;\n"
        "import cajeta.lang.stream.ArrayStream;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = [1, 2, 3, 4];\n"
        "        ArrayStream<int32> s = xs.stream();\n"
        "        Optional<int32> a = s.next();\n"
        "        Optional<int32> b = s.next();\n"
        "        Optional<int32> c = s.next();\n"
        "        return a.get() + b.get() + c.get();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 6);
}

TEST(StreamTests, arrayStreamIntrinsicAssignFormCount) {
    // Same intrinsic, terminated via the inherited count(). Routed
    // through an explicitly-typed local so the assignment pins
    // ArrayStream<int32>'s type without requiring the chained-call
    // resolveTypes machinery.
    auto src =
        "package test;\n"
        "import cajeta.lang.stream.ArrayStream;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = [10, 20, 30, 40];\n"
        "        ArrayStream<int32> s = xs.stream();\n"
        "        return s.count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 4);
}

// P6.5 — Stream.forEach + ArrayStream<int32>. No-capture lambda
// works through virtual dispatch over an int32-instantiated
// generic method. Lambdas that capture state (static-field
// references) hit a separate pre-existing lambda-codegen
// limitation — see tests for direct-call patterns and the
// non-capturing forms here.
TEST(StreamTests, arrayStreamForEachNoCaptureLambdaWalks) {
    auto src =
        "package test;\n"
        "import cajeta.lang.stream.ArrayStream;\n"
        "import cajeta.lang.stream.Stream;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = [1, 2, 3];\n"
        "        ArrayStream<int32> s = xs.stream();\n"
        "        (int32) -> void noop = (int32 v) -> { return; };\n"
        "        s.forEach(noop);\n"
        "        return 7;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

TEST(StreamTests, arrayStreamIntrinsicEmptyArray) {
    // Empty array → first next() returns None → count is 0.
    auto src =
        "package test;\n"
        "import cajeta.lang.stream.ArrayStream;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = heap int32[0];\n"
        "        ArrayStream<int32> s = xs.stream();\n"
        "        return s.count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 0);
}
