//
// A12: composition tests that exercise multiple AspectModel
// features together. Each test combines two or more of:
// @Aspect, @Component, @Inject, @PostConstruct, @Profile, plus
// the inheritance walk added in A12 for type-based pointcuts.
//
// AspectModel.md § A12: aspect-on-aspect-class, DI'd aspects,
// inheritance + advice, fiber-spanning advice. v1 covers the
// first three end-to-end; fiber-spanning advice ships when the
// fiber-aware advice context lands.
//
// All assertions go through the JIT — the drop-count side-
// channel used elsewhere is still useful for confirming advice
// fired without invasive observability machinery.
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

// Inheritance walk for type-based pointcuts: `@Before(Base.class)`
// matches methods on a subclass of Base. Pre-A12 the match was
// strict-equality only; the inheritance walk now follows the
// superClasses chain.
//
// Encoded result: target returns 5; @Before allocates+drops an
// int32[] (count bumps to 1); run() returns 5 + 100*1 = 105.
TEST(CompositionTests, advicePointcutMatchesSubclass) {
    auto src =
        "package test;\n"
        "public class Base {\n"
        "    public Base() { return; }\n"
        "}\n"
        "@Aspect public class A {\n"
        "    @Before(Base.class)\n"
        "    public static void before() {\n"
        "        int32[] tmp = heap int32[1];\n"
        "        return;\n"
        "    }\n"
        "}\n"
        "public class Child extends Base {\n"
        "    public Child() { return; }\n"
        "    public static int32 work() { return 5; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Cajeta.dropCountReset();\n"
        "        int32 r = Child.work();\n"
        "        return r + 100 * (int32) Cajeta.dropCount();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src, "test.D"), 105);
}

// Inheritance walk for an interface pointcut: `@Before(Greeter.class)`
// matches methods on classes that IMPLEMENT Greeter. Confirms
// the implementedInterfaces leg of the walk.
TEST(CompositionTests, advicePointcutMatchesInterfaceImpl) {
    auto src =
        "package test;\n"
        "public interface Greeter {\n"
        "    public int32 greet();\n"
        "}\n"
        "@Aspect public class A {\n"
        "    @Before(Greeter.class)\n"
        "    public static void before() {\n"
        "        int32[] tmp = heap int32[1];\n"
        "        return;\n"
        "    }\n"
        "}\n"
        "public class Hello implements Greeter {\n"
        "    public Hello() { return; }\n"
        "    public static int32 greet_static() { return 7; }\n"
        "    public int32 greet() { return 7; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Cajeta.dropCountReset();\n"
        "        int32 r = Hello.greet_static();\n"
        "        return r + 100 * (int32) Cajeta.dropCount();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src, "test.D"), 107);
}

// Aspect class that is ALSO a @Component. Both registries see
// it (@Aspect makes its advice fire; @Component lets it be
// injected). For v1 the aspect's instance is unused at advice
// time — advice methods are static and stateless. The point
// of this test is that registration in BOTH registries doesn't
// break either pass.
//
// Target returns 3; @Before fires (drop count 1); 3 + 100 = 103.
TEST(CompositionTests, aspectClassAlsoComponent) {
    auto src =
        "package test;\n"
        "@interface Audited { }\n"
        "@Aspect @Component public class AuditAspect {\n"
        "    public AuditAspect() { return; }\n"
        "    @Before(Audited.class)\n"
        "    public static void log() {\n"
        "        int32[] tmp = heap int32[1];\n"
        "        return;\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    @Audited public static int32 work() { return 3; }\n"
        "    public static int32 run() {\n"
        "        Cajeta.dropCountReset();\n"
        "        int32 r = work();\n"
        "        return r + 100 * (int32) Cajeta.dropCount();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src, "test.D"), 103);
}

// Full stack: @Aspect + @Component + @Inject + @PostConstruct
// all combined. Service is a @Component with an injected Dep.
// @PostConstruct stamps a marker; an aspect with @Before
// advises a marker method on Service that increments the
// drop counter. run() reads the post-construct marker as the
// base value and adds 100 per advice firing.
//
// Encoding: marker (set to 1 by @PostConstruct) + 100*1 (advice
// fires once) = 101.
TEST(CompositionTests, fullStack) {
    auto src =
        "package test;\n"
        "@interface Audited { }\n"
        "@Component public class Dep {\n"
        "    public int32 value;\n"
        "    public Dep() { value = 9; return; }\n"
        "}\n"
        "@Aspect public class A {\n"
        "    @Before(Audited.class)\n"
        "    public static void before() {\n"
        "        int32[] tmp = heap int32[1];\n"
        "        return;\n"
        "    }\n"
        "}\n"
        "@Component public class Service {\n"
        "    @Inject Dep dep;\n"
        "    public int32 marker;\n"
        "    public Service() { marker = 0; return; }\n"
        "    @PostConstruct\n"
        "    public void init() { marker = 1; return; }\n"
        "    @Audited public static int32 ping() { return 0; }\n"
        "    public static int32 run() {\n"
        "        Service s = __cajeta_inject();\n"
        "        Cajeta.dropCountReset();\n"
        "        int32 r = ping();\n"
        "        return s.marker + r + 100 * (int32) Cajeta.dropCount();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src, "test.Service"), 101);
}

// Two @Components, one extends the other; the parent is also
// a @Component. The subclass's @Inject field overrides nothing
// (it's an additional dep, not a parent's field). Verifies the
// graph build sees both components without confusing their
// shared canonical name fragments.
TEST(CompositionTests, inheritedComponentsResolveIndependently) {
    auto src =
        "package test;\n"
        "@Component public class Parent {\n"
        "    public int32 pv;\n"
        "    public Parent() { pv = 10; return; }\n"
        "}\n"
        "@Component public class Child extends Parent {\n"
        "    public int32 cv;\n"
        "    public Child() { cv = 20; return; }\n"
        "    public static int32 run() {\n"
        "        Child c = __cajeta_inject();\n"
        "        Parent p = Parent.__cajeta_inject();\n"
        "        return c.cv + p.pv;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src, "test.Child"), 30);
}
