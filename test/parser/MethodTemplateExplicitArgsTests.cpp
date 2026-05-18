// Phase 3 of MethodLevelTemplate.md: `expr.method<TypeArgs>(args)`
// call-site syntax (Form C — type args AFTER the identifier, mirrors
// `Type<args>` at the type-use site). Lets callers pin method-level
// T-vars explicitly when inference can't reach a binding (e.g. no
// value args, lambda return-type ambiguity) or simply for readability.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

int64_t runI64(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int64_t (*)()>("run");
    return fn();
}

constexpr const char* PRELUDE_MATH =
    "package test;\n"
    "import cajeta.lang.Math;\n";

} // namespace

// Static templated factory with explicit type arg. Same payload as
// the inferred form but the call site spells `<int32>` explicitly.
TEST(MethodTemplateExplicitArgsTests, staticExplicitInt32) {
    auto src = std::string(PRELUDE_MATH) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        return Math.max<int32>(3, 7);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

// Explicit int64 produces a distinct monomorphization from the
// inferred int32 one above.
TEST(MethodTemplateExplicitArgsTests, staticExplicitInt64) {
    auto src = std::string(PRELUDE_MATH) +
        "public final class D {\n"
        "    public static int64 run() {\n"
        "        return Math.max<int64>(100L, 200L);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI64(src), 200LL);
}

// Static templated method with TWO method-level type params, all
// explicit at the call site.
TEST(MethodTemplateExplicitArgsTests, staticExplicitTwoTypeArgs) {
    auto src =
        "package test;\n"
        "public class Util {\n"
        "    public static <K, V> V pickV(K k, V v) { return v; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        return Util.pickV<int64, int32>(100L, 42);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// When inference would fail (no value args constrain T), explicit
// args are the only way to reach a binding. Unblocked by two-layer
// naming: Method::getMapKey / getLlvmSymbolName append the method-arg
// suffix for instantiations, so the addMethod map keys and LLVM symbol
// for `Util::sizeOf()<int32>` are distinct from a hypothetical
// `Util::sizeOf()<int64>` even though both share the value-param
// signature `()`. The lambda-expectedType propagator stays correct
// because it already skips method-template instantiations.
TEST(MethodTemplateExplicitArgsTests, staticExplicitWhenInferenceWouldFail) {
    auto src =
        "package test;\n"
        "public class Util {\n"
        "    public static <T> int32 sizeOf() { return 42; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        return Util.sizeOf<int32>();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// Explicit args coexist with inferred args in the same compilation
// unit. Verifies cache + dispatch don't conflate the call sites.
TEST(MethodTemplateExplicitArgsTests, explicitAndInferredCoexist) {
    auto src = std::string(PRELUDE_MATH) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32 a = Math.max(3, 7);\n"
        "        int32 b = Math.max<int32>(10, 20);\n"
        "        return a + b;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 27);
}

// Explicit type args on an instance method.
TEST(MethodTemplateExplicitArgsTests, instanceExplicitTypeArg) {
    auto src =
        "package test;\n"
        "public class Box {\n"
        "    public int32 base;\n"
        "    public Box(int32 b) { this.base = b; }\n"
        "    public final <R> R passthrough(R value) { return value; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Box b = new Box(0);\n"
        "        return b.passthrough<int32>(99);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 99);
}
