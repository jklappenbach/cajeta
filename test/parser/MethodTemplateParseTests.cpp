// Phase 1 smoke for method-level templates (cajeta-docs/stdlib/
// MethodLevelTemplate.md).
//
// Phase 1 deliverables only — the grammar parses, the visitor
// captures the type parameters into the Method, the final/static
// requirement is enforced, and the method is excluded from the
// vtable. Phase 2 (monomorphization + call-site dispatch) lands
// separately; for now we exercise only declarations, not calls.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

// Compile a source string but don't lookup/call any function. We only
// care that the parse + class-build phases succeed (or throw, for the
// negative tests).
void compileOnly(const std::string& src) {
    (void) CajetaJit::compile(src, "test.D");
}

} // namespace

// A final instance method with a method-level type parameter parses
// cleanly. No call site is exercised — Phase 2 wires that up.
TEST(MethodTemplateParseTests, finalInstanceMethodWithTypeParamParses) {
    auto src =
        "package test;\n"
        "public class Foo {\n"
        "    public final <R> R passthrough(R value) { return value; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    EXPECT_NO_THROW(compileOnly(src));
}

// A static method with a method-level type parameter parses cleanly.
// Static factories are the canonical use case (Optional.Some<U> etc.).
TEST(MethodTemplateParseTests, staticMethodWithTypeParamParses) {
    auto src =
        "package test;\n"
        "public class Holder {\n"
        "    public static <U> int32 sizeOf(U value) { return 0; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    EXPECT_NO_THROW(compileOnly(src));
}

// Multiple method-level type parameters work.
TEST(MethodTemplateParseTests, multipleMethodLevelTypeParams) {
    auto src =
        "package test;\n"
        "public class Util {\n"
        "    public static <K, V> int32 zero(K k, V v) { return 0; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    EXPECT_NO_THROW(compileOnly(src));
}

// Bounded method-level params parse.
TEST(MethodTemplateParseTests, boundedMethodLevelTypeParam) {
    auto src =
        "package test;\n"
        "public class Animal { public int32 size() { return 0; } }\n"
        "public class Util {\n"
        "    public static <T extends Animal> int32 sizeOf(T a) { return a.size(); }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    EXPECT_NO_THROW(compileOnly(src));
}

// REJECTED: instance method with method-level type param but no final/
// static modifier. Surfaces the non-virtuality requirement at parse
// time with CAJETA_ERROR_METHOD_TEMPLATE_NOT_FINAL.
TEST(MethodTemplateParseTests, rejectsInstanceWithoutFinalOrStatic) {
    auto src =
        "package test;\n"
        "public class Foo {\n"
        "    public <R> R passthrough(R value) { return value; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    EXPECT_ANY_THROW(compileOnly(src));
}

// Co-existence: a non-templated method and a templated method in the
// same class both land cleanly. The non-templated one still gets a
// vtable slot; the templated one is excluded.
TEST(MethodTemplateParseTests, mixedTemplatedAndNonTemplatedMethods) {
    auto src =
        "package test;\n"
        "public class Foo {\n"
        "    public int32 plain() { return 42; }\n"
        "    public final <R> R passthrough(R value) { return value; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Foo f = new Foo();\n"
        "        return f.plain();\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 42);
}
