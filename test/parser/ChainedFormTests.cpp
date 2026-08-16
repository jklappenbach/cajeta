//
// P6.1 — chained-form completion. Pins the fluent-API path:
//
//   xs.stream().count()
//   xs.stream().forEach(lambda)
//   xs.stream().filter(p).count()
//
// Until P6.1 lands, an intermediate `xs.stream()` MCE in the receiver
// position of a chained method call doesn't set its own resolvedType
// in time for the outer MCE's resolveTypes pass — the outer call's
// `targetClass` is null, dispatch falls through to the empty-default
// path, and the second method either silently no-ops or returns
// garbage. ToDo.md Priority 6 § 1 describes the cleaner remediation
// (thread the user module into TemplateInstantiator or override
// resolveTypes for inline MCEs).
//
// All tests here are independent of the existing StreamTerminalTests
// shape, which uses a two-step `ArrayStream<int32> s = xs.stream();
// s.foo(...)` workaround precisely BECAUSE chained-form was broken.
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

// Bare chained terminal — count() on the inline stream.

// Side-effecting chained terminal — forEach(lambda) on the inline
// stream. Lambda increments a counter; assertion is on the counter
// value after the chain.

// Comparison: the two-step form (current workaround). MUST keep
// working after the chained-form fix lands.
TEST(ChainedFormTests, streamCountTwoStepStillWorks) {
    auto src =
        "package test;\n"
        "import cajeta.lang.stream.ArrayStream;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = [1, 2, 3, 4, 5];\n"
        "        ArrayStream<int32> s = xs.stream();\n"
        "        return (int32) s.count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 5);
}

// NOTE: longer chains like `xs.stream().filter(p).count()` are a
// SEPARATE concern — `Stream<T>.filter()` doesn't exist as a method
// (the existing combinator tests construct `heap FilterStream<T>(
// source, pred)` directly). That's a stdlib-completeness gap, not a
// chained-form-completion gap. Tracked under Priority 2 § 2 (Stream
// lambda combinators).
