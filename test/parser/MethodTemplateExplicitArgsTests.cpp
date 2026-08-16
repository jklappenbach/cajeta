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

// Explicit int64 produces a distinct monomorphization from the
// inferred int32 one above.

// Static templated method with TWO method-level type params, all
// explicit at the call site.

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
        "    public static int32 sizeOf<T>() { return 42; }\n"
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

// Explicit type args on an instance method.

// A templated CLASS forwarding its own type param into a BOUNDED method
// template: `Kern.scale2<E>` inside `class Pair<E extends Floating>`. The
// generic pre-pass instantiates scale2 at the wildcard sentinel — which
// must register signature-only with a throw-stub body (the bound check
// defers), while each concrete instantiation (float32, float64) re-walks
// under substitution and gets a real, correctly-typed body. Regression pin
// for the CAJETA_ERROR_METHOD_TEMPLATE_BOUND false positive this used to
// throw ("argument '?' does not satisfy numeric bound").
TEST(MethodTemplateExplicitArgsTests, classParamForwardsIntoBoundedTemplate) {
    auto src =
        "package test;\n"
        "public final class Kern {\n"
        "    public static E scale2<E extends Floating>(E v) {\n"
        "        return v + v;\n"
        "    }\n"
        "}\n"
        "public final class Pair<E extends Floating> {\n"
        "    E val;\n"
        "    public Pair(E v) { this.val = v; return; }\n"
        "    public E doubled() {\n"
        "        return Kern.scale2<E>(this.val);\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Pair<float32> a = heap Pair<float32>(2.5f);\n"
        "        float32 r = a.doubled();\n"
        "        Pair<float64> b = heap Pair<float64>(1.25);\n"
        "        float64 s = b.doubled();\n"
        "        if (r == 5.0f && s == 2.5) { return 1; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}
