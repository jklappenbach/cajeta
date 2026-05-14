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
// (@PreDestroy ships separately — it needs a runtime atexit-
// style registry that handles JIT-module lifetime correctly,
// not just a libc atexit call that would dangle on test exit.)
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
