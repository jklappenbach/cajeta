//
// A9: synthesized __cajeta_inject() codegen tests.
//
// AspectModel.md § A9 ships per-component synthesized static
// methods that lazy-create + cache a singleton, alloc + ctor +
// field-inject + return. These tests exercise the full path
// through the JIT: source declares @Components, a user-written
// static method on the component itself calls __cajeta_inject()
// bare (no receiver), and the test asserts on the return value.
//
// The receiver-eval for class-identifiers on the call side
// (`Bar.__cajeta_inject()` from inside class D) hits a pre-
// existing limitation in MethodCallExpression — a known gap
// also observed in A4 tests when the work() target lived in a
// different class. The bare-call form (`__cajeta_inject()` from
// inside the same component) goes through the structure-stack
// resolution path which works correctly today.
//
// __cajeta_inject is named with the double-underscore prefix
// (same convention as __cajeta_alloc / __cajeta_drop_*) to
// flag it as compiler-synthesized. A future user-facing entry
// (likely `Cajeta.inject_<T>()`) can land alongside cross-class
// static-call support.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src, const std::string& fqEntryClass) {
    auto jit = CajetaJit::compile(src, fqEntryClass);
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

// Single @Component with a default ctor + a field that the ctor
// initializes. __cajeta_inject() called bare from inside the
// component yields the singleton; the same method reads the field
// back. Validates the basic round-trip (alloc, vtable init, ctor
// dispatch, return-by-pointer, store to singleton, cached return).
TEST(InjectCodegenTests, singleComponentRoundTrip) {
    auto src =
        "package test;\n"
        "@Component public class Bar {\n"
        "    public int32 value;\n"
        "    public Bar() { value = 42; return; }\n"
        "    public static int32 run() {\n"
        "        Bar b = __cajeta_inject();\n"
        "        return b.value;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src, "test.Bar"), 42);
}

// A component with one @Inject field. __cajeta_inject on the
// outer component creates an inner singleton (via the inner's
// own __cajeta_inject), stores it in the outer's @Inject slot,
// returns the outer. run() reads outer.inner.value to confirm
// the field assignment landed. (Note: an intermediate local is
// used because chained dot access through a class-typed field
// hits a pre-existing DotExpression limitation — the GEP
// returns the slot address rather than auto-loading the pointer.
// Stashing into a local lets the lvalue->rvalue coercion fire
// normally at the assignment.)
TEST(InjectCodegenTests, oneInjectFieldResolves) {
    auto src =
        "package test;\n"
        "@Component public class Bar {\n"
        "    public int32 value;\n"
        "    public Bar() { value = 7; return; }\n"
        "}\n"
        "@Component public class Foo {\n"
        "    @Inject Bar bar;\n"
        "    public Foo() { return; }\n"
        "    public static int32 run() {\n"
        "        Foo f = __cajeta_inject();\n"
        "        Bar b = f.bar;\n"
        "        return b.value;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src, "test.Foo"), 7);
}

// Three-level transitive resolve: A injects B, B injects C. All
// three singletons are created on the first A.__cajeta_inject().
// Each level is loaded through a local to sidestep the class-
// field chained-dot-access gap.
TEST(InjectCodegenTests, transitiveResolutionThroughThreeLevels) {
    auto src =
        "package test;\n"
        "@Component public class C {\n"
        "    public int32 value;\n"
        "    public C() { value = 99; return; }\n"
        "}\n"
        "@Component public class B {\n"
        "    @Inject C c;\n"
        "    public B() { return; }\n"
        "}\n"
        "@Component public class A {\n"
        "    @Inject B b;\n"
        "    public A() { return; }\n"
        "    public static int32 run() {\n"
        "        A a = __cajeta_inject();\n"
        "        B mid = a.b;\n"
        "        C tail = mid.c;\n"
        "        return tail.value;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src, "test.A"), 99);
}

// Singleton identity: two calls return the same instance. Mutate
// state through the first; read it through the second. The lazy
// cache means count == 5 after the second call returns.
TEST(InjectCodegenTests, singletonIdentityAcrossCalls) {
    auto src =
        "package test;\n"
        "@Component public class Counter {\n"
        "    public int32 count;\n"
        "    public Counter() { count = 0; return; }\n"
        "    public static int32 run() {\n"
        "        Counter a = __cajeta_inject();\n"
        "        a.count = 5;\n"
        "        Counter b = __cajeta_inject();\n"
        "        return b.count;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src, "test.Counter"), 5);
}

// A @Component with no @Inject fields still gets a synthesized
// __cajeta_inject — the helper is universal across components,
// regardless of whether they have dependencies. The lazy-init
// path falls through with an empty field-injection loop.
TEST(InjectCodegenTests, componentWithoutInjectStillGetsHelper) {
    auto src =
        "package test;\n"
        "@Component public class Standalone {\n"
        "    public int32 marker;\n"
        "    public Standalone() { marker = 100; return; }\n"
        "    public static int32 run() {\n"
        "        Standalone s = __cajeta_inject();\n"
        "        return s.marker;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src, "test.Standalone"), 100);
}

// @Repository carries the same DI semantics as @Component — the
// synthesized helper fires for both. Validates the resolver
// treats them uniformly (not just at the registration boundary).
TEST(InjectCodegenTests, repositoryGetsSameHelper) {
    auto src =
        "package test;\n"
        "@Repository public class UserRepo {\n"
        "    public int32 token;\n"
        "    public UserRepo() { token = 314; return; }\n"
        "    public static int32 run() {\n"
        "        UserRepo r = __cajeta_inject();\n"
        "        return r.token;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src, "test.UserRepo"), 314);
}
