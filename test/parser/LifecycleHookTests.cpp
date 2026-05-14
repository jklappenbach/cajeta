//
// A11: @PostConstruct lifecycle hook.
//
// AspectModel.md § A11 specifies that a user method annotated
// @PostConstruct on a @Component runs once per singleton, after
// the synthesized field injection has assigned every @Inject
// field. The synthesized __cajeta_inject dispatches the hook
// between the field-injection loop and the singleton-cache
// store, so the hook sees fully-initialized @Inject fields and
// fires exactly once across any number of inject() calls.
//
// @PreDestroy ships through a runtime-internal atexit registry
// (__cajeta_atexit_push / __cajeta_run_atexit_handlers). The
// runtime's list lives in stable memory so JIT-module lifetime
// doesn't dangle the registered function pointers; tests fire
// the handlers via `Cajeta.runAtExit()` to observe side effects
// without relying on libc atexit timing.
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

// @PostConstruct on a no-@Inject component: the hook bumps an
// instance field. run() reads the field via inject() — confirms
// the hook ran by the time the cached pointer is returned.
// value starts at 0 in the ctor, then gets bumped to 99 inside
// @PostConstruct.
TEST(LifecycleHookTests, postConstructFiresOnce) {
    auto src =
        "package test;\n"
        "@Component public class Bumper {\n"
        "    public int32 value;\n"
        "    public Bumper() { value = 0; return; }\n"
        "    @PostConstruct\n"
        "    public void init() { value = 99; return; }\n"
        "    public static int32 run() {\n"
        "        Bumper b = __cajeta_inject();\n"
        "        return b.value;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src, "test.Bumper"), 99);
}

// The hook fires EXACTLY ONCE — two __cajeta_inject() calls in
// the same method yield the same singleton (cached after the
// first call), and the hook ran inside that first call. Counter
// field starts at 0, hook bumps to 1; two inject calls later
// the count is still 1 (not 2).
TEST(LifecycleHookTests, postConstructFiresExactlyOnce) {
    auto src =
        "package test;\n"
        "@Component public class Once {\n"
        "    public int32 count;\n"
        "    public Once() { count = 0; return; }\n"
        "    @PostConstruct\n"
        "    public void init() { count = count + 1; return; }\n"
        "    public static int32 run() {\n"
        "        Once a = __cajeta_inject();\n"
        "        Once b = __cajeta_inject();\n"
        "        return b.count;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src, "test.Once"), 1);
}

// @PreDestroy registered on first inject; firing Cajeta.runAtExit
// invokes the user method on the singleton. The hook bumps an
// instance field; run() returns the post-firing value.
TEST(LifecycleHookTests, preDestroyFiresOnRunAtExit) {
    auto src =
        "package test;\n"
        "@Component public class Closer {\n"
        "    public int32 closedFlag;\n"
        "    public Closer() { closedFlag = 0; return; }\n"
        "    @PreDestroy\n"
        "    public void close() { closedFlag = 7; return; }\n"
        "    public static int32 run() {\n"
        "        Closer c = __cajeta_inject();\n"
        "        Cajeta.runAtExit();\n"
        "        return c.closedFlag;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src, "test.Closer"), 7);
}

// @PreDestroy DOES NOT fire until runAtExit is called — before
// that, the flag is still at its ctor value. Mirror test that
// confirms the handler isn't run eagerly during inject().
TEST(LifecycleHookTests, preDestroyNotFiredBeforeRunAtExit) {
    auto src =
        "package test;\n"
        "@Component public class Closer {\n"
        "    public int32 closedFlag;\n"
        "    public Closer() { closedFlag = 0; return; }\n"
        "    @PreDestroy\n"
        "    public void close() { closedFlag = 7; return; }\n"
        "    public static int32 run() {\n"
        "        Closer c = __cajeta_inject();\n"
        "        return c.closedFlag;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src, "test.Closer"), 0);
}

// Registry fires LIFO. Two @Components each stamp a digit into
// a shared Tally singleton. A is injected first → registered
// first → fires SECOND under LIFO. B is injected second →
// registered second → fires FIRST under LIFO.
//   B fires:  v = 0 * 10 + 2 = 2
//   A fires:  v = 2 * 10 + 1 = 21
// If LIFO were broken (A first), the result would be
// (0 * 10 + 1) * 10 + 2 = 12.
TEST(LifecycleHookTests, preDestroyFiresLifo) {
    auto src =
        "package test;\n"
        "@Component public class Tally {\n"
        "    public int32 v;\n"
        "    public Tally() { v = 0; return; }\n"
        "}\n"
        "@Component public class A {\n"
        "    @Inject Tally t;\n"
        "    public A() { return; }\n"
        "    @PreDestroy\n"
        "    public void close() { t.v = t.v * 10 + 1; return; }\n"
        "}\n"
        "@Component public class B {\n"
        "    @Inject Tally t;\n"
        "    public B() { return; }\n"
        "    @PreDestroy\n"
        "    public void close() { t.v = t.v * 10 + 2; return; }\n"
        "    public static int32 run() {\n"
        "        A a = A.__cajeta_inject();\n"
        "        B b = __cajeta_inject();\n"
        "        Cajeta.runAtExit();\n"
        "        Tally tt = Tally.__cajeta_inject();\n"
        "        return tt.v;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src, "test.B"), 21);
}

// A @Component without @PostConstruct still works — the inject
// helper's hook-scan must no-op cleanly when no method is
// annotated. Catches an over-broad scan that would crash on
// the first non-PostConstruct method.
TEST(LifecycleHookTests, componentWithoutPostConstructStillWorks) {
    auto src =
        "package test;\n"
        "@Component public class Plain {\n"
        "    public int32 value;\n"
        "    public Plain() { value = 42; return; }\n"
        "    public static int32 run() {\n"
        "        Plain p = __cajeta_inject();\n"
        "        return p.value;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src, "test.Plain"), 42);
}

// @PostConstruct fires AFTER @Inject field assignment. The
// canonical way to observe this would be `dep.value`-from-hook,
// but the chained class-field read (foo.bar.value) hits a
// DotExpression gap unrelated to A11 — the intermediate ptr
// load isn't auto-emitted. Instead we let the hook bump a
// marker field that the test then reads; the @Inject is in
// place during the bump, just not consulted by the hook itself.
// The post-condition (marker bumped + Dep singleton lives) is
// the survivable claim for A11 — chained-access is its own
// follow-up.
TEST(LifecycleHookTests, postConstructWithInjectFieldCoexists) {
    auto src =
        "package test;\n"
        "@Component public class Dep {\n"
        "    public int32 value;\n"
        "    public Dep() { value = 7; return; }\n"
        "}\n"
        "@Component public class Holder {\n"
        "    @Inject Dep dep;\n"
        "    public int32 marker;\n"
        "    public Holder() { marker = 0; return; }\n"
        "    @PostConstruct\n"
        "    public void after() { marker = 1; return; }\n"
        "    public static int32 run() {\n"
        "        Holder h = __cajeta_inject();\n"
        "        return h.marker;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src, "test.Holder"), 1);
}
