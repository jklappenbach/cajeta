// Scopes under concurrency: a first injection builds once while other fibers
// park, a failed build releases them with a typed error, a structured child
// inherits its parent's scopes and a detached one does not
// (component-scopes plan 6.1).

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

const char* kHeader =
    "package test;\n"
    "import cajeta.time.Duration;\n"
    "import cajeta.concurrent.Fiber;\n"
    "import cajeta.concurrent.Tasks;\n"
    "import cajeta.error.RecoverableException;\n"
    "import cajeta.error.ScopeNotActiveException;\n"
    "import cajeta.error.ScopedBuildFailedException;\n"
    "class Oops extends RecoverableException {\n"
    "    Oops() { this.message = \"oops\"; this.cause = 0; }\n"
    "}\n"
    "@Scope(\"Session\") public class Session {\n"
    "    public Session() { return; }\n"
    "    public int32 cartId() {\n"
    "        Cart c = Cart.__cajeta_inject();\n"
    "        return c.id;\n"
    "    }\n"
    "}\n"
    "@Component(scope = \"Session\") public class Cart {\n"
    "    public int32 id;\n"
    "    public Cart() {\n"
    "        U.built = U.built + 1;\n"
    "        Fiber.sleep(Duration.ofMillis(30));\n"
    "        if (U.failNext) {\n"
    "            U.failNext = false;\n"
    "            throw heap Oops();\n"
    "        }\n"
    "        this.id = U.built;\n"
    "        return;\n"
    "    }\n"
    "}\n";

} // namespace

// plan 6.1.1: two fibers injecting one instance-scoped component at once get
// one instance, and its constructor ran once.
TEST(ScopeConcurrencyTests, concurrentFirstInjectionBuildsOnce) {
    auto src = std::string(kHeader) +
        "public final class U {\n"
        "    static int32 built;\n"
        "    static boolean failNext;\n"
        "    static async int32 touch(Session s) { return s.cartId(); }\n"
        "    public static int32 run() {\n"
        "        U.built = 0;\n"
        "        U.failNext = false;\n"
        "        Session s = heap Session();\n"
        "        int32 a = 0;\n"
        "        int32 b = 0;\n"
        "        scope {\n"
        "            Task<int32> ta = spawn touch(s);\n"
        "            Task<int32> tb = spawn touch(s);\n"
        "            a = await ta;\n"
        "            b = await tb;\n"
        "        }\n"
        "        return U.built * 100 + (a == b ? 1 : 0);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 101);
}

// plan 6.1.2: a throwing constructor. The builder gets its exception, the parked
// fiber gets the typed one, and a later injection builds again.
TEST(ScopeConcurrencyTests, failedBuildReleasesParkedFibers) {
    auto src = std::string(kHeader) +
        "public final class U {\n"
        "    static int32 built;\n"
        "    static boolean failNext;\n"
        "    static async int32 touch(Session s) {\n"
        "        try {\n"
        "            return s.cartId();\n"
        "        } catch (Oops e) {\n"
        "            return -1;\n"
        "        } catch (ScopedBuildFailedException e) {\n"
        "            return -2;\n"
        "        }\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        U.built = 0;\n"
        "        U.failNext = true;\n"
        "        Session s = heap Session();\n"
        "        int32 a = 0;\n"
        "        int32 b = 0;\n"
        "        scope {\n"
        "            Task<int32> ta = spawn touch(s);\n"
        "            Task<int32> tb = spawn touch(s);\n"
        "            a = await ta;\n"
        "            b = await tb;\n"
        "        }\n"
        "        int32 later = s.cartId();\n"
        "        if (a + b != -3) { return a * 100 + b; }\n"
        "        return later;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 2);
}

// plan 6.1.3: a structured child sees its parent's method-scope component, and
// the scope ends after the join with nothing leaked.
TEST(ScopeConcurrencyTests, structuredChildInheritsTheScope) {
    auto src = std::string(
        "package test;\n"
        "@Component(scope = \"Request\") public class Box {\n"
        "    public int32 v;\n"
        "    public Box() { v = 0; return; }\n"
        "}\n"
        "public final class U {\n"
        "    static async int32 read() {\n"
        "        Box b = Box.__cajeta_inject();\n"
        "        return b.v;\n"
        "    }\n"
        "    @Scope(\"Request\")\n"
        "    static int32 handle(int32 n) {\n"
        "        Box b = Box.__cajeta_inject();\n"
        "        b.v = n;\n"
        "        int32 got = 0;\n"
        "        scope {\n"
        "            Task<int32> t = spawn read();\n"
        "            got = await t;\n"
        "        }\n"
        "        return got;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int32 r = U.handle(1);\n"
        "        int64 live = Cajeta.liveCount();\n"
        "        int32 i = 0;\n"
        "        while (i < 50) { r = U.handle(5); i = i + 1; }\n"
        "        if (Cajeta.liveCount() != live) { return -1; }\n"
        "        return r;\n"
        "    }\n"
        "}\n");
    EXPECT_EQ(runI32(src), 5);
}

// plan 6.1.4: a detached task sees no scope it was spawned in.
TEST(ScopeConcurrencyTests, detachedTaskSeesNoScope) {
    auto src = std::string(
        "package test;\n"
        "import cajeta.time.Duration;\n"
        "import cajeta.concurrent.Fiber;\n"
        "import cajeta.concurrent.Tasks;\n"
        "import cajeta.error.ScopeNotActiveException;\n"
        "@Component(scope = \"Request\") public class Box {\n"
        "    public Box() { return; }\n"
        "}\n"
        "public final class U {\n"
        "    static int32 outcome;\n"
        "    static async void lonely() {\n"
        "        try {\n"
        "            Box b = Box.__cajeta_inject();\n"
        "            U.outcome = 1;\n"
        "        } catch (ScopeNotActiveException e) {\n"
        "            U.outcome = 2;\n"
        "        }\n"
        "        return;\n"
        "    }\n"
        "    @Scope(\"Request\")\n"
        "    static void handle() {\n"
        "        Box b = Box.__cajeta_inject();\n"
        "        detach lonely();\n"
        "        return;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        U.outcome = 0;\n"
        "        U.handle();\n"
        "        int32 i = 0;\n"
        "        while (U.outcome == 0 && i < 2000) { Fiber.sleep(Duration.ofMillis(1)); i = i + 1; }\n"
        "        return U.outcome;\n"
        "    }\n"
        "}\n");
    EXPECT_EQ(runI32(src), 2);
}

// plan 6.1.5: a singleton raced by two fibers is built once.
TEST(ScopeConcurrencyTests, singletonRacedByTwoFibersIsBuiltOnce) {
    auto src = std::string(
        "package test;\n"
        "import cajeta.time.Duration;\n"
        "import cajeta.concurrent.Fiber;\n"
        "import cajeta.concurrent.Tasks;\n"
        "@Component public class Slow {\n"
        "    public int32 id;\n"
        "    public Slow() {\n"
        "        U.built = U.built + 1;\n"
        "        Fiber.sleep(Duration.ofMillis(30));\n"
        "        this.id = U.built;\n"
        "        return;\n"
        "    }\n"
        "}\n"
        "public final class U {\n"
        "    static int32 built;\n"
        "    static async int32 get() {\n"
        "        Slow s = Slow.__cajeta_inject();\n"
        "        return s.id;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        U.built = 0;\n"
        "        int32 a = 0;\n"
        "        int32 b = 0;\n"
        "        scope {\n"
        "            Task<int32> ta = spawn get();\n"
        "            Task<int32> tb = spawn get();\n"
        "            a = await ta;\n"
        "            b = await tb;\n"
        "        }\n"
        "        return U.built * 100 + (a == b ? 1 : 0);\n"
        "    }\n"
        "}\n");
    EXPECT_EQ(runI32(src), 101);
}
