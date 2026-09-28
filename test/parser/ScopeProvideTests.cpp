// Values produced at runtime: `Components.provide(#v)` places a value in the
// active scope its type declares, and a later injection gets it
// (component-scopes plan 7.1).

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.U");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

const char* kPrincipal =
    "package test;\n"
    "import cajeta.aot.Components;\n"
    "import cajeta.error.ScopeNotActiveException;\n"
    "import cajeta.error.ScopedBuildFailedException;\n"
    "import cajeta.error.NotProvidedException;\n"
    "@Component(scope = \"Request\") public class Principal {\n"
    "    public int32 subject;\n"
    "    public Principal(int32 subject) { this.subject = subject; return; }\n"
    "    @PreDestroy\n"
    "    public void bye() { U.destroyed = U.destroyed + 1; return; }\n"
    "}\n";

} // namespace

// plan 7.1.1: a provided value is what later injections get, and it is freed at
// the scope's end.
TEST(ScopeProvideTests, providedValueIsInjectedAndFreed) {
    auto src = std::string(kPrincipal) +
        "public final class U {\n"
        "    static int32 destroyed;\n"
        "    static int32 who() {\n"
        "        Principal p = Principal.__cajeta_inject();\n"
        "        return p.subject;\n"
        "    }\n"
        "    @Scope(\"Request\")\n"
        "    static int32 handle(int32 n) {\n"
        "        Principal p = heap Principal(n);\n"
        "        Components.provide(#p);\n"
        "        return U.who();\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        U.destroyed = 0;\n"
        "        int32 r = U.handle(3);\n"
        "        int64 live = Cajeta.liveCount();\n"
        "        int32 i = 0;\n"
        "        while (i < 100) { r = U.handle(i); i = i + 1; }\n"
        "        if (Cajeta.liveCount() != live) { return -1; }\n"
        "        return r * 1000 + U.destroyed;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 99 * 1000 + 101);
}

// plan 7.1.2: injecting a provided type before it was provided throws, naming
// the type and the scope.
TEST(ScopeProvideTests, injectionBeforeProvideThrows) {
    auto src = std::string(kPrincipal) +
        "public final class U {\n"
        "    static int32 destroyed;\n"
        "    @Scope(\"Request\")\n"
        "    static int32 handle() {\n"
        "        try {\n"
        "            Principal p = Principal.__cajeta_inject();\n"
        "            return -1;\n"
        "        } catch (NotProvidedException e) {\n"
        "            int64 a = e.message.indexOf(\"test.Principal\");\n"
        "            int64 b = e.message.indexOf(\"Request\");\n"
        "            if (a < 0 || b < 0) { return -2; }\n"
        "            return 7;\n"
        "        }\n"
        "    }\n"
        "    public static int32 run() { return U.handle(); }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

// plan 7.1.3: a second provide in one scope throws, and the refused value is
// freed rather than leaked.
TEST(ScopeProvideTests, secondProvideThrows) {
    auto src = std::string(kPrincipal) +
        "public final class U {\n"
        "    static int32 destroyed;\n"
        "    @Scope(\"Request\")\n"
        "    static int32 handle() {\n"
        "        Principal a = heap Principal(1);\n"
        "        Components.provide(#a);\n"
        "        Principal b = heap Principal(2);\n"
        "        try {\n"
        "            Components.provide(#b);\n"
        "            return -1;\n"
        "        } catch (ScopedBuildFailedException e) {\n"
        "            Principal p = Principal.__cajeta_inject();\n"
        "            return p.subject;\n"
        "        }\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int32 r = U.handle();\n"
        "        int64 live = Cajeta.liveCount();\n"
        "        int32 i = 0;\n"
        "        while (i < 50) { r = U.handle(); i = i + 1; }\n"
        "        if (Cajeta.liveCount() != live) { return -9; }\n"
        "        return r;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// Outside every activation, provide throws ScopeNotActiveException.
TEST(ScopeProvideTests, provideOutsideTheScopeThrows) {
    auto src = std::string(kPrincipal) +
        "public final class U {\n"
        "    static int32 destroyed;\n"
        "    @Scope(\"Request\")\n"
        "    static void noop() { return; }\n"
        "    public static int32 run() {\n"
        "        Principal p = heap Principal(1);\n"
        "        try {\n"
        "            Components.provide(#p);\n"
        "            return -1;\n"
        "        } catch (ScopeNotActiveException e) {\n"
        "            return 4;\n"
        "        }\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 4);
}
