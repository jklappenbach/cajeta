// Lifetime rules between scopes: a field may hold a component only when the
// component's scope provably outlives the holder; otherwise the site injects a
// `Scoped<T>` handle and resolves on each `get()` (component-scopes plan 5.1).

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/error/Exception.h"
#include "../xpu/KernelLoweringProbe.h"

#include <cstdint>
#include <string>

using cajeta::Compiler;
using cajeta::CajetaModule;
using cajeta::Exception;
using cajeta::xpu::probe::compileForInspection;
using cajeta_test::CajetaJit;

namespace {

std::string resolveError(const std::string& src, const std::string& fq) {
    Compiler compiler;
    try {
        compileForInspection(compiler, src, fq, "scoperule");
        CajetaModule::resolveDependencyGraph();
    } catch (Exception& e) {
        return e.getErrorId() + ": " + e.getMessage();
    }
    return "";
}

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.U");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

const char* kScopes =
    "package test;\n"
    "import cajeta.collection.ArrayList;\n"
    "public class Pipe {\n"
    "    @Scope(\"Request\") public void run() { return; }\n"
    "}\n"
    "@Scope(\"Session\") public class Session {\n"
    "    public Session() { return; }\n"
    "}\n"
    "@Component(scope = \"Request\") public class Cart {\n"
    "    public Cart() { return; }\n"
    "}\n"
    "@Component(scope = \"Session\") public class Prefs {\n"
    "    public Prefs() { return; }\n"
    "}\n"
    "@Component public class Clock {\n"
    "    public Clock() { return; }\n"
    "}\n";

} // namespace

// plan 5.1.1: a singleton may not hold a scoped component.
TEST(ScopeRuleTests, singletonHoldingRequestFails) {
    auto err = resolveError(std::string(kScopes) +
        "@Component public class Service {\n"
        "    @Inject Cart cart;\n"
        "    public Service() { return; }\n"
        "}\n", "test.Pipe");
    EXPECT_EQ(err.rfind("CAJETA_ERROR_SCOPE_WIDENING", 0), 0u) << err;
    EXPECT_NE(err.find("Scoped<"), std::string::npos) << err;
}

// plan 5.1.1: unrelated scopes may not hold each other.
TEST(ScopeRuleTests, unrelatedScopesFail) {
    auto err = resolveError(std::string(kScopes) +
        "@Component(scope = \"Request\") public class Checkout {\n"
        "    @Inject Prefs prefs;\n"
        "    public Checkout() { return; }\n"
        "}\n", "test.Pipe");
    EXPECT_EQ(err.rfind("CAJETA_ERROR_SCOPE_WIDENING", 0), 0u) << err;
}

// plan 5.1.1: a Transient holder is owned by its caller and may outlive a scope.
TEST(ScopeRuleTests, transientHoldingRequestFails) {
    auto err = resolveError(std::string(kScopes) +
        "@Component(scope = \"Transient\") public class Tok {\n"
        "    @Inject Cart cart;\n"
        "    public Tok() { return; }\n"
        "}\n", "test.Pipe");
    EXPECT_EQ(err.rfind("CAJETA_ERROR_SCOPE_WIDENING", 0), 0u) << err;
}

// plan 5.1.1: same scope, and a singleton target, compile.
TEST(ScopeRuleTests, sameScopeAndSingletonTargetsCompile) {
    auto err = resolveError(std::string(kScopes) +
        "@Component(scope = \"Request\") public class Checkout {\n"
        "    @Inject Cart cart;\n"
        "    @Inject Clock clock;\n"
        "    public Checkout() { return; }\n"
        "}\n", "test.Pipe");
    EXPECT_EQ(err, "");
}

// plan 5.1.1: a declared `within` licenses the outward field.
TEST(ScopeRuleTests, declaredWithinLicensesTheField) {
    auto err = resolveError(std::string(kScopes) +
        "public class Dispatch {\n"
        "    @Scope(value = \"Message\", within = \"Session\")\n"
        "    public void onMessage() { return; }\n"
        "}\n"
        "@Component(scope = \"Message\") public class Frame {\n"
        "    @Inject Prefs prefs;\n"
        "    public Frame() { return; }\n"
        "}\n", "test.Pipe");
    EXPECT_EQ(err, "");
}

// plan 5.1.1: a method scope on an instance-scope class is within it (spec 2.4).
TEST(ScopeRuleTests, methodOnAnchorClassLicensesTheField) {
    auto err = resolveError(
        "package test;\n"
        "@Scope(\"Connection\") public class Conn {\n"
        "    public Conn() { return; }\n"
        "    @Scope(\"Message\") public void onMessage() { return; }\n"
        "}\n"
        "@Component(scope = \"Connection\") public class Subs {\n"
        "    public Subs() { return; }\n"
        "}\n"
        "@Component(scope = \"Message\") public class Frame {\n"
        "    @Inject Subs subs;\n"
        "    public Frame() { return; }\n"
        "}\n", "test.Conn");
    EXPECT_EQ(err, "");
}

// plan 5.2.4: a multibinding is filled once, like a field, so a scoped member is
// refused.
TEST(ScopeRuleTests, scopedMultibindingMemberFails) {
    auto err = resolveError(std::string(kScopes) +
        "public interface Stage { }\n"
        "@Component(scope = \"Request\") public class Scribble implements Stage {\n"
        "    public Scribble() { return; }\n"
        "}\n"
        "@Component public class Pipeline {\n"
        "    @Inject ArrayList<Stage> stages;\n"
        "    public Pipeline() { return; }\n"
        "}\n", "test.Pipe");
    EXPECT_EQ(err.rfind("CAJETA_ERROR_SCOPE_WIDENING", 0), 0u) << err;
}

// plan 5.1.2: entering a scope whose `within` is inactive throws on entry.
TEST(ScopeRuleTests, withinIsCheckedOnEntry) {
    auto src =
        "package test;\n"
        "import cajeta.error.ScopeNotActiveException;\n"
        "@Scope(\"Connection\") public class Conn {\n"
        "    public Conn() { return; }\n"
        "    public int32 deliver() { return Dispatch.onMessage(); }\n"
        "}\n"
        "public final class Dispatch {\n"
        "    @Scope(value = \"Message\", within = \"Connection\")\n"
        "    public static int32 onMessage() { return 5; }\n"
        "}\n"
        "public final class U {\n"
        "    public static int32 run() {\n"
        "        Conn c = heap Conn();\n"
        "        int32 inside = c.deliver();\n"
        "        try {\n"
        "            int32 outside = Dispatch.onMessage();\n"
        "            return -1;\n"
        "        } catch (ScopeNotActiveException e) {\n"
        "            int64 a = e.message.indexOf(\"Message\");\n"
        "            int64 b = e.message.indexOf(\"Connection\");\n"
        "            if (a < 0 || b < 0) { return -2; }\n"
        "            return inside;\n"
        "        }\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 5);
}

// plan 5.1.3: a Scoped<T> handle resolves in the scope active at each get().
TEST(ScopeRuleTests, scopedHandleResolvesAtEachCall) {
    auto src =
        "package test;\n"
        "import cajeta.aot.Scoped;\n"
        "import cajeta.error.ScopeNotActiveException;\n"
        "@Component(scope = \"Request\") public class Cart {\n"
        "    public int32 v;\n"
        "    public Cart() { v = 0; return; }\n"
        "}\n"
        "@Component public class Service {\n"
        "    @Inject Scoped<Cart> cart;\n"
        "    public Service() { return; }\n"
        "}\n"
        "public final class U {\n"
        "    @Scope(\"Request\")\n"
        "    static int32 handle(int32 n) {\n"
        "        Service s = Service.__cajeta_inject();\n"
        "        Cart a = s.cart.get();\n"
        "        a.v = a.v + n;\n"
        "        Cart b = s.cart.get();\n"
        "        return b.v;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int32 x = U.handle(5);\n"
        "        int64 live = Cajeta.liveCount();\n"
        "        int32 y = 0;\n"
        "        int32 i = 0;\n"
        "        while (i < 50) { y = U.handle(7); i = i + 1; }\n"
        "        if (Cajeta.liveCount() != live) { return -1; }\n"
        "        Service s = Service.__cajeta_inject();\n"
        "        try {\n"
        "            Cart c = s.cart.get();\n"
        "            return -2;\n"
        "        } catch (ScopeNotActiveException e) {\n"
        "            return x * 100 + y;\n"
        "        }\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 507);
}
