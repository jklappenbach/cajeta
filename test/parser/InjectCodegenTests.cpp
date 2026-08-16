//
// A9: synthesized __cajeta_inject() codegen tests.
//
// AspectModel.md § A9 ships per-component synthesized static
// methods that lazy-create + cache a singleton, alloc + ctor +
// field-inject + return. These tests exercise the full path
// through the JIT: source declares @Components, a user-written
// static method calls __cajeta_inject() (bare or via the class
// name now that cross-class static dispatch works), and the
// test asserts on the return value.
//
// __cajeta_inject is named with the double-underscore prefix
// (same convention as __cajeta_alloc / __cajeta_drop_*) to
// flag it as compiler-synthesized. The cross-class form
// (`Bar.__cajeta_inject()` from a non-component caller) is
// validated separately in CrossClassStaticCallTests.
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

// A component with one @Inject field. __cajeta_inject on the
// outer component creates an inner singleton (via the inner's
// own __cajeta_inject), stores it in the outer's @Inject slot,
// returns the outer. run() reads outer.bar.value via chained
// dot access — the DotExpression auto-load through class-typed
// intermediates makes this work directly.

// Three-level transitive resolve: A injects B, B injects C. All
// three singletons are created on the first A.__cajeta_inject().
// Direct chained access a.b.c.value walks through two class-
// typed intermediates.

// Singleton identity: two calls return the same instance. Mutate
// state through the first; read it through the second. The lazy
// cache means count == 5 after the second call returns.

// A @Component with no @Inject fields still gets a synthesized
// __cajeta_inject — the helper is universal across components,
// regardless of whether they have dependencies. The lazy-init
// path falls through with an empty field-injection loop.

// Class-typed local from a field READ is a borrow (no drop).
// Pre-fix this double-freed the singleton: the local registered
// a drop, the receiving scope dropped, and the second @Inject
// site dropped again. Test source: take a class-typed local
// from a class field, then call __cajeta_inject again — under
// the old behavior, the second call returns the same pointer
// and a later scope-exit double-free crashed. Now the local is
// a borrow, the singleton lives, the second call returns the
// same (still-alive) instance, observed via mutation.
TEST(InjectCodegenTests, classTypedLocalFromFieldReadIsBorrow) {
    auto src =
        "package test;\n"
        "@Component public class Inner {\n"
        "    public int32 v;\n"
        "    public Inner() { v = 11; return; }\n"
        "}\n"
        "@Component public class Outer {\n"
        "    @Inject Inner inner;\n"
        "    public Outer() { return; }\n"
        "    public static int32 run() {\n"
        "        Outer o = __cajeta_inject();\n"
        "        Inner i = o.inner;\n"
        "        i.v = 33;\n"
        "        Inner j = Inner.__cajeta_inject();\n"
        "        return j.v;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src, "test.Outer"), 33);
}

// @Repository carries the same DI semantics as @Component — the
// synthesized helper fires for both. Validates the resolver
// treats them uniformly (not just at the registration boundary).
