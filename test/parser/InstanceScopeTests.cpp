// Instance scopes: `@Scope("Session")` on a class makes each instance an anchor.
// A component with `scope = "Session"` is one per instance, survives between
// calls, and is freed with the instance (component-scopes plan 4.1).

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

const char* kSession =
    "@Scope(\"Session\") public class Session {\n"
    "    public int32 id;\n"
    "    public Session(int32 id) { this.id = id; return; }\n"
    "    public int32 add(int32 n) {\n"
    "        Cart c = Cart.__cajeta_inject();\n"
    "        c.v = c.v + n;\n"
    "        return c.v;\n"
    "    }\n"
    "    private int32 hidden() {\n"
    "        Cart c = Cart.__cajeta_inject();\n"
    "        return c.v;\n"
    "    }\n"
    "    public int32 viaPrivate() { return this.hidden(); }\n"
    "    public static int32 outside() {\n"
    "        Cart c = Cart.__cajeta_inject();\n"
    "        return c.v;\n"
    "    }\n"
    "}\n"
    "@Component(scope = \"Session\") public class Cart {\n"
    "    public int32 v;\n"
    "    public Cart() { v = 0; return; }\n"
    "    @PreDestroy\n"
    "    public void bye() { U.destroyed = U.destroyed + 1; return; }\n"
    "}\n";

std::string unit(const std::string& body, const std::string& imports = "") {
    return std::string("package test;\n") + imports + kSession + body;
}

} // namespace

// plan 4.1.1: one component per instance, surviving between calls.
TEST(InstanceScopeTests, survivesBetweenCallsAndDiffersPerInstance) {
    auto src = unit(
        "public final class U {\n"
        "    static int32 destroyed;\n"
        "    public static int32 run() {\n"
        "        Session a = heap Session(1);\n"
        "        Session b = heap Session(2);\n"
        "        int32 x = a.add(5);\n"
        "        int32 y = a.add(7);\n"
        "        int32 z = b.add(1);\n"
        "        return x * 10000 + y * 100 + z;\n"
        "    }\n"
        "}\n");
    EXPECT_EQ(runI32(src), 5 * 10000 + 12 * 100 + 1);
}

// plan 4.1.2: dropping the anchor frees its components and runs @PreDestroy.
TEST(InstanceScopeTests, droppingTheAnchorFreesItsComponents) {
    auto src = unit(
        "public final class U {\n"
        "    static int32 destroyed;\n"
        "    static int32 once(int32 n) {\n"
        "        Session s = heap Session(n);\n"
        "        int32 a = s.add(n);\n"
        "        return a;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        U.destroyed = 0;\n"
        "        int32 r = U.once(1);\n"
        "        int64 live = Cajeta.liveCount();\n"
        "        int32 tables = Cajeta.scopeTables();\n"
        "        int32 i = 0;\n"
        "        while (i < 100) { r = U.once(i); i = i + 1; }\n"
        "        if (Cajeta.liveCount() != live) { return -1; }\n"
        "        int32 after = Cajeta.scopeTables();\n"
        "        if (after != tables) { return -2; }\n"
        "        return U.destroyed;\n"
        "    }\n"
        "}\n");
    EXPECT_EQ(runI32(src), 101);
}

// plan 4.1.3: a private method is reached through a public one, which already
// made the instance current; a static method makes nothing current.
TEST(InstanceScopeTests, privateAndStaticMethods) {
    auto src = unit(
        "public final class U {\n"
        "    static int32 destroyed;\n"
        "    public static int32 run() {\n"
        "        Session s = heap Session(1);\n"
        "        int32 a = s.add(4);\n"
        "        int32 b = s.viaPrivate();\n"
        "        try {\n"
        "            int32 c = Session.outside();\n"
        "            return -1;\n"
        "        } catch (ScopeNotActiveException e) {\n"
        "            return a * 10 + b;\n"
        "        }\n"
        "    }\n"
        "}\n", "import cajeta.error.ScopeNotActiveException;\n");
    EXPECT_EQ(runI32(src), 44);
}

// plan 4.1.3: a subclass instance is an anchor, including its own methods.
TEST(InstanceScopeTests, subclassInstanceIsAnAnchor) {
    auto src = unit(
        "public class Premium extends Session {\n"
        "    public Premium(int32 id) { this.id = id; return; }\n"
        "    public int32 bonus() {\n"
        "        Cart c = Cart.__cajeta_inject();\n"
        "        c.v = c.v + 100;\n"
        "        return c.v;\n"
        "    }\n"
        "}\n"
        "public final class U {\n"
        "    static int32 destroyed;\n"
        "    public static int32 run() {\n"
        "        Premium p = heap Premium(3);\n"
        "        int32 a = p.add(2);\n"
        "        int32 b = p.bonus();\n"
        "        return b;\n"
        "    }\n"
        "}\n");
    EXPECT_EQ(runI32(src), 102);
}

// plan 4.1.4: not current during its own constructor; current after it.
TEST(InstanceScopeTests, notCurrentDuringTheConstructor) {
    auto src = std::string(
        "package test;\n"
        "import cajeta.error.ScopeNotActiveException;\n"
        "@Scope(\"Session\") public class Session {\n"
        "    public int32 seen;\n"
        "    public Session() {\n"
        "        try {\n"
        "            Cart c = Cart.__cajeta_inject();\n"
        "            this.seen = 1;\n"
        "        } catch (ScopeNotActiveException e) {\n"
        "            this.seen = 2;\n"
        "        }\n"
        "        return;\n"
        "    }\n"
        "    public int32 probe() {\n"
        "        Cart c = Cart.__cajeta_inject();\n"
        "        return 3;\n"
        "    }\n"
        "}\n"
        "@Component(scope = \"Session\") public class Cart {\n"
        "    public int32 v;\n"
        "    public Cart() { v = 0; return; }\n"
        "}\n"
        "public final class U {\n"
        "    public static int32 run() {\n"
        "        Session s = heap Session();\n"
        "        return s.seen * 10 + s.probe();\n"
        "    }\n"
        "}\n");
    EXPECT_EQ(runI32(src), 23);
}

// plan 4.1.5: a method scope published on an instance-scope class is within it,
// so its component may inject the instance-scoped one directly.
TEST(InstanceScopeTests, methodScopeOnAnchorClassIsWithinIt) {
    auto src = std::string(
        "package test;\n"
        "@Scope(\"Connection\") public class Conn {\n"
        "    public Conn() { return; }\n"
        "    @Scope(\"Message\")\n"
        "    public int32 onMessage(int32 n) {\n"
        "        Frame f = Frame.__cajeta_inject();\n"
        "        f.subs.count = f.subs.count + n;\n"
        "        return f.subs.count;\n"
        "    }\n"
        "}\n"
        "@Component(scope = \"Connection\") public class Subs {\n"
        "    public int32 count;\n"
        "    public Subs() { count = 0; return; }\n"
        "}\n"
        "@Component(scope = \"Message\") public class Frame {\n"
        "    @Inject Subs subs;\n"
        "    public Frame() { return; }\n"
        "}\n"
        "public final class U {\n"
        "    public static int32 run() {\n"
        "        Conn c = heap Conn();\n"
        "        int32 a = c.onMessage(2);\n"
        "        int32 b = c.onMessage(3);\n"
        "        return a * 10 + b;\n"
        "    }\n"
        "}\n");
    EXPECT_EQ(runI32(src), 25);
}
