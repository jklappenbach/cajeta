// Method scopes: a component with `scope = "Request"` is one instance per
// activation of the method that publishes `@Scope("Request")`, freed when the
// activation returns or throws (component-scopes plan 3.1).

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

const char* kCart =
    "@Component(scope = \"Request\") public class Cart {\n"
    "    public int32 v;\n"
    "    public Cart() { v = 0; return; }\n"
    "}\n";

std::string unit(const std::string& body, const std::string& imports = "") {
    return std::string("package test;\n") + imports + kCart + body;
}

} // namespace

// plan 3.1.1: one instance per activation, a fresh one per activation.
TEST(MethodScopeTests, oneInstancePerActivation) {
    auto src = unit(
        "public final class U {\n"
        "    @Scope(\"Request\")\n"
        "    static int32 handle(int32 n) {\n"
        "        Cart a = Cart.__cajeta_inject();\n"
        "        Cart b = Cart.__cajeta_inject();\n"
        "        a.v = a.v + n;\n"
        "        return b.v;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int32 x = U.handle(5);\n"
        "        int32 y = U.handle(7);\n"
        "        return x * 100 + y;\n"
        "    }\n"
        "}\n");
    EXPECT_EQ(runI32(src), 507);
}

// plan 3.1.2: a recursive activation gets its own instance.
TEST(MethodScopeTests, innermostActivationWins) {
    auto src = unit(
        "public final class U {\n"
        "    @Scope(\"Request\")\n"
        "    static int32 depth(int32 n) {\n"
        "        Cart c = Cart.__cajeta_inject();\n"
        "        c.v = c.v + 10 + n;\n"
        "        if (n > 0) {\n"
        "            int32 inner = U.depth(n - 1);\n"
        "        }\n"
        "        Cart again = Cart.__cajeta_inject();\n"
        "        return again.v;\n"
        "    }\n"
        "    public static int32 run() { return U.depth(3); }\n"
        "}\n");
    EXPECT_EQ(runI32(src), 13);
}

// plan 3.1.3: freed with @PreDestroy on return and on throw, with no leak.
TEST(MethodScopeTests, freedOnReturnAndThrow) {
    auto src =
        "package test;\n"
        "import cajeta.error.RecoverableException;\n"
        "class Oops extends RecoverableException {\n"
        "    Oops() { this.message = \"oops\"; this.cause = 0; }\n"
        "}\n"
        "@Component(scope = \"Request\") public class Cart {\n"
        "    public int32 v;\n"
        "    public Cart() { v = 0; return; }\n"
        "    @PreDestroy\n"
        "    public void bye() { U.destroyed = U.destroyed + 1; return; }\n"
        "}\n"
        "public final class U {\n"
        "    static int32 destroyed;\n"
        "    @Scope(\"Request\")\n"
        "    static int32 handle() {\n"
        "        Cart c = Cart.__cajeta_inject();\n"
        "        c.v = 1;\n"
        "        return c.v;\n"
        "    }\n"
        "    @Scope(\"Request\")\n"
        "    static int32 fails() {\n"
        "        Cart c = Cart.__cajeta_inject();\n"
        "        c.v = 2;\n"
        "        throw heap Oops();\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        U.destroyed = 0;\n"
        "        int32 w = U.handle();\n"
        "        try { int32 z = U.fails(); } catch (Oops e) { w = w + 0; }\n"
        "        int64 live = Cajeta.liveCount();\n"
        "        int32 i = 0;\n"
        "        while (i < 100) {\n"
        "            int32 r = U.handle();\n"
        "            try { int32 z = U.fails(); } catch (Oops e) { r = r + 0; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        int64 after = Cajeta.liveCount();\n"
        "        if (after != live) { return -1; }\n"
        "        return U.destroyed;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 202);
}

// plan 3.1.4: outside every activation the injection throws, naming both.
TEST(MethodScopeTests, outsideTheScopeThrowsTyped) {
    auto src = unit(
        "public final class U {\n"
        "    @Scope(\"Request\")\n"
        "    static int32 handle() { return 0; }\n"
        "    public static int32 run() {\n"
        "        try {\n"
        "            Cart c = Cart.__cajeta_inject();\n"
        "            return -1;\n"
        "        } catch (ScopeNotActiveException e) {\n"
        "            int64 a = e.message.indexOf(\"test.Cart\");\n"
        "            int64 b = e.message.indexOf(\"Request\");\n"
        "            if (a < 0 || b < 0) { return -2; }\n"
        "            return 7;\n"
        "        }\n"
        "    }\n"
        "}\n", "import cajeta.error.ScopeNotActiveException;\n");
    EXPECT_EQ(runI32(src), 7);
}

// plan 3.1.5: a scoped component's fields: a same-scope component and a
// singleton. Ending the scope frees the holder and the cart once, never the
// singleton, which stays usable.
TEST(MethodScopeTests, scopedComponentFieldsAreBorrowed) {
    auto src = unit(
        "@Component public class Clock {\n"
        "    public int32 ticks;\n"
        "    public Clock() { ticks = 0; return; }\n"
        "}\n"
        "@Component(scope = \"Request\") public class Checkout {\n"
        "    @Inject Cart cart;\n"
        "    @Inject Clock clock;\n"
        "    public Checkout() { return; }\n"
        "}\n"
        "public final class U {\n"
        "    @Scope(\"Request\")\n"
        "    static int32 handle(int32 n) {\n"
        "        Checkout k = Checkout.__cajeta_inject();\n"
        "        Cart c = Cart.__cajeta_inject();\n"
        "        c.v = n;\n"
        "        k.clock.ticks = k.clock.ticks + 1;\n"
        "        return k.cart.v;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int32 first = U.handle(3);\n"
        "        int64 live = Cajeta.liveCount();\n"
        "        int32 i = 0;\n"
        "        int32 last = 0;\n"
        "        while (i < 50) { last = U.handle(i); i = i + 1; }\n"
        "        if (Cajeta.liveCount() != live) { return -1; }\n"
        "        Clock clock = Clock.__cajeta_inject();\n"
        "        return first * 10000 + last * 100 + clock.ticks;\n"
        "    }\n"
        "}\n");
    EXPECT_EQ(runI32(src), 3 * 10000 + 49 * 100 + 51);
}

// plan 3.1.6: publishing on an interface method makes each implementation an
// anchor.
TEST(MethodScopeTests, interfaceMethodPublishesForImplementations) {
    auto src = unit(
        "public interface Handler {\n"
        "    @Scope(\"Request\")\n"
        "    int32 handle(int32 n);\n"
        "}\n"
        "public class H implements Handler {\n"
        "    public H() { return; }\n"
        "    public int32 handle(int32 n) {\n"
        "        Cart a = Cart.__cajeta_inject();\n"
        "        Cart b = Cart.__cajeta_inject();\n"
        "        a.v = a.v + n;\n"
        "        return b.v;\n"
        "    }\n"
        "}\n"
        "public final class U {\n"
        "    public static int32 run() {\n"
        "        Handler h = heap H();\n"
        "        int32 x = h.handle(5);\n"
        "        int32 y = h.handle(7);\n"
        "        return x * 100 + y;\n"
        "    }\n"
        "}\n");
    EXPECT_EQ(runI32(src), 507);
}

// plan 3.1.6: an override of an anchored method is an anchor too.
TEST(MethodScopeTests, overrideOfAnchoredMethodIsAnAnchor) {
    auto src = unit(
        "public class Base {\n"
        "    public Base() { return; }\n"
        "    @Scope(\"Request\")\n"
        "    public int32 go(int32 n) { return 0; }\n"
        "}\n"
        "public class Sub extends Base {\n"
        "    public Sub() { return; }\n"
        "    public int32 go(int32 n) {\n"
        "        Cart a = Cart.__cajeta_inject();\n"
        "        a.v = a.v + n;\n"
        "        return a.v;\n"
        "    }\n"
        "}\n"
        "public final class U {\n"
        "    public static int32 run() {\n"
        "        Sub s = heap Sub();\n"
        "        int32 x = s.go(5);\n"
        "        int32 y = s.go(7);\n"
        "        return x * 100 + y;\n"
        "    }\n"
        "}\n");
    EXPECT_EQ(runI32(src), 507);
}

// plan 3.1.7: an activation that injects nothing allocates no table.
TEST(MethodScopeTests, unusedActivationAllocatesNothing) {
    auto src = unit(
        "public final class U {\n"
        "    @Scope(\"Request\")\n"
        "    static int32 idle(int32 n) {\n"
        "        int32 tables = Cajeta.scopeTables();\n"
        "        return tables;\n"
        "    }\n"
        "    @Scope(\"Request\")\n"
        "    static int32 busy() {\n"
        "        Cart c = Cart.__cajeta_inject();\n"
        "        int32 tables = Cajeta.scopeTables();\n"
        "        return tables;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int32 before = Cajeta.scopeTables();\n"
        "        int32 idle = U.idle(1);\n"
        "        int32 busy = U.busy();\n"
        "        int32 after = Cajeta.scopeTables();\n"
        "        return before * 1000 + idle * 100 + busy * 10 + after;\n"
        "    }\n"
        "}\n");
    EXPECT_EQ(runI32(src), 10);
}

// plan 3.2.4: a component declaring Transient builds fresh on every call, and
// the caller owns and frees it.
TEST(MethodScopeTests, transientComponentIsFreshAndOwned) {
    auto src = unit(
        "@Component(scope = \"Transient\") public class Tok {\n"
        "    public int32 v;\n"
        "    public Tok() { v = 0; return; }\n"
        "}\n"
        "public final class U {\n"
        "    @Scope(\"Request\")\n"
        "    static void noop() { return; }\n"
        "    static int32 once() {\n"
        "        Tok a = Tok.__cajeta_inject();\n"
        "        Tok b = Tok.__cajeta_inject();\n"
        "        a.v = 9;\n"
        "        return b.v;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int32 first = U.once();\n"
        "        int64 live = Cajeta.liveCount();\n"
        "        int32 i = 0;\n"
        "        while (i < 100) { first = first + U.once(); i = i + 1; }\n"
        "        if (Cajeta.liveCount() != live) { return -1; }\n"
        "        return first;\n"
        "    }\n"
        "}\n");
    EXPECT_EQ(runI32(src), 0);
}

// A scoped holder owns its Owner and Transient fields and frees them at the
// scope's end; a Transient field is built fully, its own fields filled.
TEST(MethodScopeTests, scopedHolderFreesItsFreshFields) {
    auto src = unit(
        "@Component public class Clock {\n"
        "    public int32 ticks;\n"
        "    public Clock() { ticks = 4; return; }\n"
        "}\n"
        "public class Meter {\n"
        "    public int32 n;\n"
        "    public Meter() { n = 2; return; }\n"
        "}\n"
        "@Component public class Gauge {\n"
        "    public int32 g;\n"
        "    public Gauge() { g = 3; return; }\n"
        "}\n"
        "@Component(scope = \"Transient\") public class Tok {\n"
        "    @Inject Clock clock;\n"
        "    public Tok() { return; }\n"
        "}\n"
        "@Component(scope = \"Request\") public class Checkout {\n"
        "    @Inject(scope = \"Owner\") Gauge gauge;\n"
        "    @Inject Tok tok;\n"
        "    public Checkout() { return; }\n"
        "}\n"
        "public final class U {\n"
        "    @Scope(\"Request\")\n"
        "    static int32 handle() {\n"
        "        Checkout k = Checkout.__cajeta_inject();\n"
        "        return k.gauge.g * 10 + k.tok.clock.ticks;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int32 r = U.handle();\n"
        "        int64 live = Cajeta.liveCount();\n"
        "        int32 i = 0;\n"
        "        while (i < 100) { r = U.handle(); i = i + 1; }\n"
        "        if (Cajeta.liveCount() != live) { return -1; }\n"
        "        return r;\n"
        "    }\n"
        "}\n");
    EXPECT_EQ(runI32(src), 34);
}
