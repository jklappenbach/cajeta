// Same method name can carry a non-templated AND a templated overload
// in the same class. Pins that the resolver routes correctly based on
// whether the call site provides explicit type args.
//
//   public static int32 foo(int32 x)       → non-templated; called as `foo(1)`
//   public static int32 foo<T>(T x)       → templated;     called as `foo<int32>(1)`
//
// Before the Method::getMapKey fix in this commit, registering both
// on the same class triggered addMethod's duplicate-static throw
// because the two methods had identical map keys.

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

// Non-templated and templated overloads coexist. Bare call site
// `foo(7)` resolves to the non-templated overload.
TEST(MethodTemplateOverloadTests, bareCallPicksNonTemplated) {
    auto src =
        "package test;\n"
        "public class Util {\n"
        "    public static int32 foo(int32 x) { return 1; }\n"
        "    public static int32 foo<T>(T value) { return 2; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        return Util.foo(7);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// Explicit type args route to the templated overload.
TEST(MethodTemplateOverloadTests, explicitTypeArgPicksTemplated) {
    auto src =
        "package test;\n"
        "public class Util {\n"
        "    public static int32 foo(int32 x) { return 1; }\n"
        "    public static int32 foo<T>(T value) { return 2; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        return Util.foo<int32>(7);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 2);
}

// Templated overload still works on its own (no non-templated peer).
TEST(MethodTemplateOverloadTests, templatedAloneStillResolves) {
    auto src =
        "package test;\n"
        "public class Util {\n"
        "    public static int32 foo<T>(T value) { return 9; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        return Util.foo<int32>(7);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 9);
}
