//
// @Factory assisted-injection call-site threading (AspectModel.md
// § @Factory R3 / aot-di Unit 4b). A consumer injects the @Factory and
// calls a provider method passing ONLY the assisted args; the compiler
// splices the graph-resolved @Inject args into their declared positions.
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

// 4b.1.1 — the consumer injects the factory and calls make(7) with only
// the assisted arg; the @Inject Pool is threaded automatically, and the
// product reflects pool.tag (3) + tenant (7) = 10. Init-beyond-ctor runs.

// 4b.1.2 — interleaving: make(int32 a, @Inject Pool, int32 b) called as
// make(2, 5) places the user args in the two assisted slots (a=2, b=5)
// and splices the injected Pool into the middle slot. Expect
// 2*1000 + pool.tag(100) + 5 = 2105.
TEST(FactoryAssistedTests, assistedInterleavedWithInjectedMiddle) {
    auto src =
        "package test;\n"
        "@Component public class Pool {\n"
        "    public int32 tag;\n"
        "    public Pool() { tag = 100; return; }\n"
        "}\n"
        "public class Connection {\n"
        "    public int32 value;\n"
        "    public Connection() { value = 0; return; }\n"
        "}\n"
        "@Factory public class ConnFactory {\n"
        "    #Connection make(int32 a, @Inject Pool pool, int32 b) {\n"
        "        Connection c = heap Connection();\n"
        "        c.value = a * 1000 + pool.tag + b;\n"
        "        return c;\n"
        "    }\n"
        "}\n"
        "@Component public class App {\n"
        "    @Inject ConnFactory factory;\n"
        "    public App() { return; }\n"
        "    public static int32 run() {\n"
        "        App a = __cajeta_inject();\n"
        "        Connection c = a.factory.make(2, 5);\n"
        "        return c.value;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src, "test.App"), 2105);
}
