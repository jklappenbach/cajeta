// Factory products in a published scope: a `@Factory` method carrying
// `@Component(scope = "Name")` provides a component of that scope, built on its
// first injection in an activation, adopted by the scope's table and dropped at
// the scope's end (component-scopes spec §11, plan Unit 10).

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

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.U");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

std::string resolveError(const std::string& src, const std::string& fq) {
    Compiler compiler;
    try {
        compileForInspection(compiler, src, fq, "scopedfactory");
        CajetaModule::resolveDependencyGraph();
    } catch (Exception& e) {
        return e.getErrorId() + ": " + e.getMessage();
    }
    return "";
}

// Engine stands in for a library type: no annotation, built only by the factory.
// `U.calls` counts factory calls, `U.dropped` the drops, `U.log` the order.
const char* kParts =
    "package test;\n"
    "import cajeta.aot.Components;\n"
    "import cajeta.aot.Scoped;\n"
    "import cajeta.error.ScopeNotActiveException;\n"
    "import cajeta.error.ScopedBuildFailedException;\n"
    "import cajeta.error.NotProvidedException;\n"
    "public class Engine {\n"
    "    public int32 ctx;\n"
    "    public Engine(int32 ctx) { this.ctx = ctx; U.log = U.log * 10 + 1; return; }\n"
    "    public ~Engine() { U.dropped = U.dropped + 1; U.log = U.log * 10 + 4; }\n"
    "}\n"
    "@Component(scope = \"Request\") public class Cfg {\n"
    "    public int32 ctx;\n"
    "    public Cfg() { this.ctx = 7; return; }\n"
    "}\n"
    "@Factory public class Parts {\n"
    "    @Component(scope = \"Request\") #Engine engine(@Inject Cfg c) {\n"
    "        U.calls = U.calls + 1;\n"
    "        Engine e = heap Engine(c.ctx);\n"
    "        return e;\n"
    "    }\n"
    "}\n"
    "@Component(scope = \"Request\") public class UseA {\n"
    "    @Inject Engine e;\n"
    "    public UseA() { return; }\n"
    "}\n"
    "@Component(scope = \"Request\") public class UseB {\n"
    "    @Inject Engine e;\n"
    "    public UseB() { return; }\n"
    "}\n";

const char* kStatics =
    "    static int32 calls;\n"
    "    static int32 dropped;\n"
    "    static int32 log;\n";

} // namespace

// 10.1.1: two injections in one activation are one product, built once.
TEST(ScopedFactoryTests, oneProductPerActivation) {
    auto src = std::string(kParts) +
        "public final class U {\n" + kStatics +
        "    @Scope(\"Request\")\n"
        "    static int32 handle() {\n"
        "        UseA a = UseA.__cajeta_inject();\n"
        "        UseB b = UseB.__cajeta_inject();\n"
        "        int32 same = a.e == b.e ? 1 : 0;\n"
        "        return same * 1000 + a.e.ctx * 10 + U.calls;\n"
        "    }\n"
        "    public static int32 run() { U.calls = 0; return U.handle(); }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1000 + 70 + 1);
}

// 10.1.2: each activation builds its own product and drops it at its end, on a
// return and on a throw; the live count balances.
TEST(ScopedFactoryTests, eachActivationBuildsAndDropsItsOwn) {
    auto src = std::string(kParts) +
        "public final class U {\n" + kStatics +
        "    @Scope(\"Request\")\n"
        "    static int32 handle() {\n"
        "        UseA a = UseA.__cajeta_inject();\n"
        "        return a.e.ctx;\n"
        "    }\n"
        "    @Scope(\"Request\")\n"
        "    static int32 fails() {\n"
        "        UseA a = UseA.__cajeta_inject();\n"
        "        throw heap Exception(\"after the build\");\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        U.calls = 0;\n"
        "        U.dropped = 0;\n"
        "        U.handle();\n"
        "        int64 live = Cajeta.liveCount();\n"
        "        int32 i = 0;\n"
        "        while (i < 20) { U.handle(); i = i + 1; }\n"
        "        try { U.fails(); } catch (Exception e) { }\n"
        "        if (Cajeta.liveCount() != live) { return -1; }\n"
        "        return U.calls * 100 + U.dropped;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 22 * 100 + 22);
}

// 10.1.3: a product in an instance-published scope lives as long as the anchor
// instance, and two anchors hold two.
TEST(ScopedFactoryTests, anInstanceScopeHoldsItsOwnProduct) {
    auto src =
        "package test;\n"
        "public class Engine {\n"
        "    public int32 ctx;\n"
        "    public Engine(int32 ctx) { this.ctx = ctx; return; }\n"
        "    public ~Engine() { U.dropped = U.dropped + 1; }\n"
        "}\n"
        "@Factory public class Parts {\n"
        "    @Component(scope = \"Cabra\") #Engine engine() {\n"
        "        U.calls = U.calls + 1;\n"
        "        Engine e = heap Engine(U.calls);\n"
        "        return e;\n"
        "    }\n"
        "}\n"
        "@Component(scope = \"Cabra\") public class Use {\n"
        "    @Inject Engine e;\n"
        "    public Use() { return; }\n"
        "}\n"
        "@Scope(\"Cabra\") public class Cabra {\n"
        "    public Cabra() { return; }\n"
        "    public int32 ctx() {\n"
        "        Use u = Use.__cajeta_inject();\n"
        "        return u.e.ctx;\n"
        "    }\n"
        "}\n"
        "public final class U {\n"
        "    static int32 calls;\n"
        "    static int32 dropped;\n"
        "    public static int32 run() {\n"
        "        U.calls = 0;\n"
        "        U.dropped = 0;\n"
        "        Cabra a = heap Cabra();\n"
        "        int32 r = 0;\n"
        "        {\n"
        "            Cabra b = heap Cabra();\n"
        "            r = a.ctx() * 100 + a.ctx() * 10 + b.ctx();\n"
        "        }\n"
        "        int32 afterB = U.dropped;\n"
        "        int32 again = a.ctx();\n"
        "        return r * 1000 + afterB * 100 + again * 10 + U.calls;\n"
        "    }\n"
        "}\n";
    // a builds 1, b builds 2; b's drop is the only one; a keeps its 1.
    EXPECT_EQ(runI32(src), 112 * 1000 + 1 * 100 + 1 * 10 + 2);
}

// 10.1.5: a product whose factory injects another product is built after it and
// dropped before it.
TEST(ScopedFactoryTests, aDependentProductIsBuiltAfterAndDroppedBefore) {
    auto src = std::string(kParts) +
        "public class Tmpl {\n"
        "    public int32 k;\n"
        "    public Tmpl(int32 k) { this.k = k; U.log = U.log * 10 + 2; return; }\n"
        "    public ~Tmpl() { U.log = U.log * 10 + 3; }\n"
        "}\n"
        "@Factory public class More {\n"
        "    @Component(scope = \"Request\") #Tmpl tmpl(@Inject Engine e) {\n"
        "        Tmpl t = heap Tmpl(e.ctx);\n"
        "        return t;\n"
        "    }\n"
        "}\n"
        "@Component(scope = \"Request\") public class UseT {\n"
        "    @Inject Tmpl t;\n"
        "    public UseT() { return; }\n"
        "}\n"
        "public final class U {\n" + kStatics +
        "    @Scope(\"Request\")\n"
        "    static int32 handle() {\n"
        "        UseT u = UseT.__cajeta_inject();\n"
        "        return u.t.k;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        U.log = 0;\n"
        "        int32 k = U.handle();\n"
        "        return k * 10000 + U.log;\n"
        "    }\n"
        "}\n";
    // Engine built (1), Tmpl built (2), Tmpl dropped (3), Engine dropped (4).
    EXPECT_EQ(runI32(src), 7 * 10000 + 1234);
}

// 10.1.6: a factory parameter in a narrower or unrelated scope is a lifetime
// error; a singleton or the same scope compiles.
TEST(ScopedFactoryTests, aNarrowerParameterIsAWideningError) {
    auto err = resolveError(
        "package test;\n"
        "public class Pipe {\n"
        "    @Scope(\"Request\") public void run() { return; }\n"
        "}\n"
        "@Scope(\"Session\") public class Session {\n"
        "    public Session() { return; }\n"
        "}\n"
        "@Component(scope = \"Request\") public class Cart {\n"
        "    public Cart() { return; }\n"
        "}\n"
        "public class Engine {\n"
        "    public Engine() { return; }\n"
        "}\n"
        "@Factory public class Parts {\n"
        "    @Component(scope = \"Session\") #Engine engine(@Inject Cart c) {\n"
        "        Engine e = heap Engine();\n"
        "        return e;\n"
        "    }\n"
        "}\n", "test.Pipe");
    EXPECT_EQ(err.rfind("CAJETA_ERROR_SCOPE_WIDENING", 0), 0u) << err;
    EXPECT_NE(err.find("Session"), std::string::npos) << err;
    EXPECT_NE(err.find("Request"), std::string::npos) << err;
}

TEST(ScopedFactoryTests, aSingletonOrSameScopeParameterCompiles) {
    auto err = resolveError(
        "package test;\n"
        "public class Pipe {\n"
        "    @Scope(\"Request\") public void run() { return; }\n"
        "}\n"
        "@Component(scope = \"Request\") public class Cart {\n"
        "    public Cart() { return; }\n"
        "}\n"
        "@Component public class Clock {\n"
        "    public Clock() { return; }\n"
        "}\n"
        "public class Engine {\n"
        "    public Engine() { return; }\n"
        "}\n"
        "@Factory public class Parts {\n"
        "    @Component(scope = \"Request\") #Engine engine(@Inject Cart c, @Inject Clock k) {\n"
        "        Engine e = heap Engine();\n"
        "        return e;\n"
        "    }\n"
        "}\n", "test.Pipe");
    EXPECT_EQ(err, "");
}

// 10.1.7: a null product is absent: an optional site gets null, a plain site
// throws NotProvidedException, and the factory runs once per activation.
TEST(ScopedFactoryTests, aNullProductIsAbsent) {
    auto src =
        "package test;\n"
        "import cajeta.error.NotProvidedException;\n"
        "public class Tmpl {\n"
        "    public Tmpl() { return; }\n"
        "}\n"
        "@Factory public class Parts {\n"
        "    @Component(scope = \"Request\") #Tmpl tmpl() {\n"
        "        U.calls = U.calls + 1;\n"
        "        return null;\n"
        "    }\n"
        "}\n"
        "@Component(scope = \"Request\") public class Maybe {\n"
        "    @Inject(optional = true) Tmpl t;\n"
        "    public Maybe() { return; }\n"
        "}\n"
        "@Component(scope = \"Request\") public class Needs {\n"
        "    @Inject Tmpl t;\n"
        "    public Needs() { return; }\n"
        "}\n"
        "public final class U {\n"
        "    static int32 calls;\n"
        "    @Scope(\"Request\")\n"
        "    static int32 handle() {\n"
        "        Maybe m = Maybe.__cajeta_inject();\n"
        "        int32 absent = m.t == null ? 1 : 0;\n"
        "        int32 threw = 0;\n"
        "        try {\n"
        "            Needs n = Needs.__cajeta_inject();\n"
        "        } catch (NotProvidedException e) {\n"
        "            int64 a = e.message.indexOf(\"test.Tmpl\");\n"
        "            threw = a >= 0 ? 1 : 2;\n"
        "        }\n"
        "        return absent * 100 + threw * 10 + U.calls;\n"
        "    }\n"
        "    public static int32 run() { U.calls = 0; return U.handle(); }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 100 + 10 + 1);
}

// 10.1.9: a product type that is also a @Component is ambiguous.
TEST(ScopedFactoryTests, aProductThatIsAlsoAComponentIsAmbiguous) {
    auto err = resolveError(
        "package test;\n"
        "public class Pipe {\n"
        "    @Scope(\"Request\") public void run() { return; }\n"
        "}\n"
        "@Component(scope = \"Request\") public class Engine {\n"
        "    public Engine() { return; }\n"
        "}\n"
        "@Factory public class Parts {\n"
        "    @Component(scope = \"Request\") #Engine engine() {\n"
        "        Engine e = heap Engine();\n"
        "        return e;\n"
        "    }\n"
        "}\n"
        "@Component(scope = \"Request\") public class Use {\n"
        "    @Inject Engine e;\n"
        "    public Use() { return; }\n"
        "}\n", "test.Pipe");
    EXPECT_EQ(err.rfind("CAJETA_ERROR_DI_AMBIGUOUS", 0), 0u) << err;
}
