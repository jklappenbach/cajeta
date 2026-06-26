//
// @Factory provider-accessor codegen (AspectModel.md § @Factory / aot-di
// Unit 4a). Full JIT path: an all-injected provider lowers to a
// synthesized accessor that calls the factory method with graph-resolved
// @Inject args; a consumer's @Inject of the product routes to it.
//
//   - @Singleton (default): built once; shared across @Inject sites.
//   - @Transient: a fresh product per @Inject site.
//   - init-beyond-ctor (R2) runs; injected collaborators are visible in
//     the provider body.
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

// 4a.1.1 — the product is built by the factory method: the @Inject
// collaborator (Pool) is threaded into make(), and init-beyond-ctor
// (c.value = pool.tag + 10) runs. App.@Inject Connection routes to the
// accessor. Expect 3 + 10 = 13.
TEST(FactoryCodegenTests, providerSingletonBuiltViaFactory) {
    auto src =
        "package test;\n"
        "@Component public class Pool {\n"
        "    public int32 tag;\n"
        "    public Pool() { tag = 3; return; }\n"
        "}\n"
        "public class Connection {\n"
        "    public int32 value;\n"
        "    public Connection() { value = 0; return; }\n"
        "}\n"
        "@Factory public class ConnFactory {\n"
        "    #Connection make(@Inject Pool pool) {\n"
        "        Connection c = heap Connection();\n"
        "        c.value = pool.tag + 10;\n"
        "        return c;\n"
        "    }\n"
        "}\n"
        "@Component public class App {\n"
        "    @Inject Connection conn;\n"
        "    public App() { return; }\n"
        "    public static int32 run() {\n"
        "        App a = __cajeta_inject();\n"
        "        return a.conn.value;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src, "test.App"), 13);
}

// 4a.1.2a — @Singleton provider: two @Inject sites share one instance.
// Mutate via c1, read via c2 → 50 (same object).
TEST(FactoryCodegenTests, providerSingletonIdentityShared) {
    auto src =
        "package test;\n"
        "public class Connection {\n"
        "    public int32 value;\n"
        "    public Connection() { value = 1; return; }\n"
        "}\n"
        "@Factory public class ConnFactory {\n"
        "    #Connection make() {\n"
        "        Connection c = heap Connection();\n"
        "        return c;\n"
        "    }\n"
        "}\n"
        "@Component public class App {\n"
        "    @Inject Connection c1;\n"
        "    @Inject Connection c2;\n"
        "    public App() { return; }\n"
        "    public static int32 run() {\n"
        "        App a = __cajeta_inject();\n"
        "        a.c1.value = 50;\n"
        "        return a.c2.value;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src, "test.App"), 50);
}

// 4a.1.2b — @Transient provider: each @Inject site gets a fresh product.
// Mutate via c1, read via c2 → 1 (distinct object, untouched, keeps its
// ctor value). The contrast with the singleton case (50) proves the
// scope distinction.
TEST(FactoryCodegenTests, providerTransientDistinct) {
    auto src =
        "package test;\n"
        "public class Connection {\n"
        "    public int32 value;\n"
        "    public Connection() { value = 1; return; }\n"
        "}\n"
        "@Factory public class ConnFactory {\n"
        "    @Transient #Connection make() {\n"
        "        Connection c = heap Connection();\n"
        "        return c;\n"
        "    }\n"
        "}\n"
        "@Component public class App {\n"
        "    @Inject Connection c1;\n"
        "    @Inject Connection c2;\n"
        "    public App() { return; }\n"
        "    public static int32 run() {\n"
        "        App a = __cajeta_inject();\n"
        "        a.c1.value = 50;\n"
        "        return a.c2.value;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src, "test.App"), 1);
}
