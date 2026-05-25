// Wildcard lints from cajeta-docs/LintRules.md. The four rules
// (`wildcard-materialize-in-loop`, `wildcard-crosses-hot-boundary`,
// `wildcard-field-in-small-class`, `discarded-wildcard-next`) surface
// performance/correctness hazards that the wildcard erasure model
// introduces; they're advisory only, suppressible via @SuppressLint.
//
// Each rule has three coverage shapes: positive (fires when expected),
// negative (doesn't fire when the structural trigger isn't met), and
// suppression (silenced by @SuppressLint on the enclosing
// method/class).

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

constexpr const char* PRELUDE =
    "package test;\n"
    "import cajeta.lang.stream.ArrayStream;\n";

} // namespace

// ---------------------------------------------------------------------
// wildcard-field-in-small-class
// ---------------------------------------------------------------------

// A class field typed `Stream<?>` triggers the lint at the class's
// declaration site. The diagnostic names the field and the rule.
TEST(WildcardLintTests, fieldInSmallClassFires) {
    auto src = std::string(PRELUDE) +
        "public class Holder {\n"
        "    ArrayStream<?> source;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    testing::internal::CaptureStderr();
    auto jit = CajetaJit::compile(src, "test.D");
    std::string err = testing::internal::GetCapturedStderr();
    EXPECT_NE(err.find("[wildcard-field-in-small-class]"), std::string::npos)
        << "expected wildcard-field warning for Holder.source, got: " << err;
    EXPECT_NE(err.find("source"), std::string::npos);
}

// A concrete instantiation `Stream<int32>` doesn't trigger — only
// wildcard instantiations route through virtual-drop dispatch.
TEST(WildcardLintTests, fieldInSmallClassNoFireOnConcrete) {
    auto src = std::string(PRELUDE) +
        "public class Holder {\n"
        "    ArrayStream<int32> source;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    testing::internal::CaptureStderr();
    auto jit = CajetaJit::compile(src, "test.D");
    std::string err = testing::internal::GetCapturedStderr();
    EXPECT_EQ(err.find("[wildcard-field-in-small-class]"), std::string::npos)
        << "did not expect wildcard-field warning for concrete field, got: " << err;
}

// `@SuppressLint("wildcard-field-in-small-class")` on the class
// silences the warning. The use case from the rule spec: a long-lived
// or pooled holder where the wildcard field is intentional.
TEST(WildcardLintTests, fieldInSmallClassSuppressed) {
    auto src = std::string(PRELUDE) +
        "@SuppressLint(\"wildcard-field-in-small-class\")\n"
        "public class Holder {\n"
        "    ArrayStream<?> source;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    testing::internal::CaptureStderr();
    auto jit = CajetaJit::compile(src, "test.D");
    std::string err = testing::internal::GetCapturedStderr();
    EXPECT_EQ(err.find("[wildcard-field-in-small-class]"), std::string::npos)
        << "@SuppressLint should have silenced the warning, got: " << err;
}

// ---------------------------------------------------------------------
// wildcard-materialize-in-loop
// ---------------------------------------------------------------------

// `s.next()` on a `Stream<?>` receiver inside a while-loop triggers
// the lint. Element-producing method names — `next`, `get` — are the
// trigger set; non-element names don't fire.
TEST(WildcardLintTests, materializeInLoopFires) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = new int32[2];\n"
        "        xs[0] = 1; xs[1] = 2;\n"
        "        ArrayStream<int32> s = xs.stream();\n"
        "        ArrayStream<?> cursor = s;\n"
        "        int32 c = 0;\n"
        "        while (c < 1) {\n"
        "            Optional<?> o = cursor.next();\n"
        "            c = c + 1;\n"
        "        }\n"
        "        return c;\n"
        "    }\n"
        "}\n";
    testing::internal::CaptureStderr();
    auto jit = CajetaJit::compile(src, "test.D");
    std::string err = testing::internal::GetCapturedStderr();
    EXPECT_NE(err.find("[wildcard-materialize-in-loop]"), std::string::npos)
        << "expected wildcard-materialize-in-loop on cursor.next() in while-loop, got: "
        << err;
}

// Same shape but outside any loop — lint must not fire. Regression
// guard for the hasLoopContext() check.
TEST(WildcardLintTests, materializeOutsideLoopNoFire) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = new int32[1];\n"
        "        xs[0] = 1;\n"
        "        ArrayStream<int32> s = xs.stream();\n"
        "        ArrayStream<?> cursor = s;\n"
        "        Optional<?> o = cursor.next();\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    testing::internal::CaptureStderr();
    auto jit = CajetaJit::compile(src, "test.D");
    std::string err = testing::internal::GetCapturedStderr();
    EXPECT_EQ(err.find("[wildcard-materialize-in-loop]"), std::string::npos)
        << "did not expect lint outside any loop, got: " << err;
}

// @SuppressLint on the enclosing method silences the warning even
// though the structural trigger fires.
TEST(WildcardLintTests, materializeInLoopSuppressed) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    @SuppressLint(\"wildcard-materialize-in-loop\")\n"
        "    public static int32 run() {\n"
        "        int32[] xs = new int32[2];\n"
        "        xs[0] = 1; xs[1] = 2;\n"
        "        ArrayStream<int32> s = xs.stream();\n"
        "        ArrayStream<?> cursor = s;\n"
        "        int32 c = 0;\n"
        "        while (c < 1) {\n"
        "            Optional<?> o = cursor.next();\n"
        "            c = c + 1;\n"
        "        }\n"
        "        return c;\n"
        "    }\n"
        "}\n";
    testing::internal::CaptureStderr();
    auto jit = CajetaJit::compile(src, "test.D");
    std::string err = testing::internal::GetCapturedStderr();
    EXPECT_EQ(err.find("[wildcard-materialize-in-loop]"), std::string::npos)
        << "@SuppressLint should have silenced the warning, got: " << err;
}

// ---------------------------------------------------------------------
// wildcard-crosses-hot-boundary
// ---------------------------------------------------------------------

// A method whose return type is wildcard-typed, called inside a loop,
// trips the lint. `cursor.next()` returns `Optional<?>` (wildcard) so
// both this rule and `wildcard-materialize-in-loop` fire — but we
// assert only on this rule's id here.
TEST(WildcardLintTests, crossesHotBoundaryFires) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = new int32[2];\n"
        "        xs[0] = 1; xs[1] = 2;\n"
        "        ArrayStream<int32> s = xs.stream();\n"
        "        ArrayStream<?> cursor = s;\n"
        "        int32 c = 0;\n"
        "        while (c < 1) {\n"
        "            Optional<?> o = cursor.next();\n"
        "            c = c + 1;\n"
        "        }\n"
        "        return c;\n"
        "    }\n"
        "}\n";
    testing::internal::CaptureStderr();
    auto jit = CajetaJit::compile(src, "test.D");
    std::string err = testing::internal::GetCapturedStderr();
    EXPECT_NE(err.find("[wildcard-crosses-hot-boundary]"), std::string::npos)
        << "expected wildcard-crosses-hot-boundary, got: " << err;
}

// Outside a loop — must not fire.
TEST(WildcardLintTests, crossesHotBoundaryOutsideLoopNoFire) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = new int32[1];\n"
        "        xs[0] = 1;\n"
        "        ArrayStream<int32> s = xs.stream();\n"
        "        ArrayStream<?> cursor = s;\n"
        "        Optional<?> o = cursor.next();\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    testing::internal::CaptureStderr();
    auto jit = CajetaJit::compile(src, "test.D");
    std::string err = testing::internal::GetCapturedStderr();
    EXPECT_EQ(err.find("[wildcard-crosses-hot-boundary]"), std::string::npos)
        << "did not expect lint outside any loop, got: " << err;
}

// @SuppressLint silences the boundary warning specifically — sibling
// rules like `wildcard-materialize-in-loop` are not affected by this
// suppression (covered separately above).
TEST(WildcardLintTests, crossesHotBoundarySuppressed) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    @SuppressLint(\"wildcard-crosses-hot-boundary\")\n"
        "    public static int32 run() {\n"
        "        int32[] xs = new int32[2];\n"
        "        xs[0] = 1; xs[1] = 2;\n"
        "        ArrayStream<int32> s = xs.stream();\n"
        "        ArrayStream<?> cursor = s;\n"
        "        int32 c = 0;\n"
        "        while (c < 1) {\n"
        "            Optional<?> o = cursor.next();\n"
        "            c = c + 1;\n"
        "        }\n"
        "        return c;\n"
        "    }\n"
        "}\n";
    testing::internal::CaptureStderr();
    auto jit = CajetaJit::compile(src, "test.D");
    std::string err = testing::internal::GetCapturedStderr();
    EXPECT_EQ(err.find("[wildcard-crosses-hot-boundary]"), std::string::npos)
        << "@SuppressLint should have silenced the warning, got: " << err;
}

// ---------------------------------------------------------------------
// discarded-wildcard-next
// ---------------------------------------------------------------------

// `cursor.next();` in statement position with the result thrown away
// trips the lint. The boxed Optional<?> is allocated and discarded.
TEST(WildcardLintTests, discardedWildcardNextFires) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = new int32[1];\n"
        "        xs[0] = 1;\n"
        "        ArrayStream<int32> s = xs.stream();\n"
        "        ArrayStream<?> cursor = s;\n"
        "        cursor.next();\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    testing::internal::CaptureStderr();
    auto jit = CajetaJit::compile(src, "test.D");
    std::string err = testing::internal::GetCapturedStderr();
    EXPECT_NE(err.find("[discarded-wildcard-next]"), std::string::npos)
        << "expected discarded-wildcard-next on bare cursor.next(), got: " << err;
}

// Same call, but the result is bound to a local — must not fire. The
// rule is about *discarded* results.
TEST(WildcardLintTests, boundWildcardNextNoFire) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = new int32[1];\n"
        "        xs[0] = 1;\n"
        "        ArrayStream<int32> s = xs.stream();\n"
        "        ArrayStream<?> cursor = s;\n"
        "        Optional<?> o = cursor.next();\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    testing::internal::CaptureStderr();
    auto jit = CajetaJit::compile(src, "test.D");
    std::string err = testing::internal::GetCapturedStderr();
    EXPECT_EQ(err.find("[discarded-wildcard-next]"), std::string::npos)
        << "did not expect lint when result is bound, got: " << err;
}

// @SuppressLint silences the warning even when the result is discarded.
TEST(WildcardLintTests, discardedWildcardNextSuppressed) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    @SuppressLint(\"discarded-wildcard-next\")\n"
        "    public static int32 run() {\n"
        "        int32[] xs = new int32[1];\n"
        "        xs[0] = 1;\n"
        "        ArrayStream<int32> s = xs.stream();\n"
        "        ArrayStream<?> cursor = s;\n"
        "        cursor.next();\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    testing::internal::CaptureStderr();
    auto jit = CajetaJit::compile(src, "test.D");
    std::string err = testing::internal::GetCapturedStderr();
    EXPECT_EQ(err.find("[discarded-wildcard-next]"), std::string::npos)
        << "@SuppressLint should have silenced the warning, got: " << err;
}
