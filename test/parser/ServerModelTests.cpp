// NET-9.2 — cajeta.io.net.ServerModel selection.
//
// A ServerModel names which accept stack to materialize — fiber-per-connection
// (Model A, NET-4.2) or the event-driven shared-pool (Model B, NET-4.3) —
// and, for Model B, the worker-pool size. The bind path branches on it
// (ServerModel.bindServer -> Server.bind for Model A vs SharedPoolServer.bind
// for Model B). That decision logic is socket-free and JIT-runnable
// deterministically.
//
// Each test compiles a small Cajeta run() that builds a ServerModel, inspects
// the selection it encodes, and returns an int32 sentinel (1 on success, a
// distinct negative per failed sub-check).
//
// The HTTP server that rides these models (and its builder wiring) moved to
// the external dev.cajeta.http library; its model-threading coverage lives in
// that repo's server suite.

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

// Wrap a method body in a class importing the net model type.
// The body must `return` int32.
int32_t runI32(const std::string& body) {
    std::string src =
        "package test;\n"
        "import cajeta.lang.String;\n"
        "import cajeta.io.net.ServerModel;\n"
        "public final class M {\n"
        "    public static int32 run() {\n"
        "        " + body + "\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.M");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

// --- Model A — fiber-per-connection selection ---------------------------

// The default model is fiber-per-connection (Model A): isFiberPerConnection,
// not isSharedPool, pool size 0 (unused).
TEST(ServerModelTests, fiberPerConnectionSelectsModelA) {
    EXPECT_EQ(runI32(
        "ServerModel m #= ServerModel.fiberPerConnection();\n"
        "if (!m.isFiberPerConnection()) return -1;\n"
        "if (m.isSharedPool()) return -2;\n"
        "if (m.getPoolSize() != 0) return -3;\n"
        "if (m.kind != ServerModel.FIBER_PER_CONNECTION_KIND) return -4;\n"
        "return 1;"), 1);
}

// --- Model B — shared-pool selection ------------------------------------

// SharedPool(8) selects Model B and carries the worker-pool size through.
TEST(ServerModelTests, sharedPoolSelectsModelBWithPoolSize) {
    EXPECT_EQ(runI32(
        "ServerModel m #= ServerModel.sharedPool(8);\n"
        "if (!m.isSharedPool()) return -1;\n"
        "if (m.isFiberPerConnection()) return -2;\n"
        "if (m.getPoolSize() != 8) return -3;\n"
        "if (m.kind != ServerModel.SHARED_POOL_KIND) return -4;\n"
        "return 1;"), 1);
}

// A non-positive pool size is normalized up to 1 (the SharedPoolServer floor),
// so the model never names a zero-worker pool.
TEST(ServerModelTests, sharedPoolFloorsNonPositiveToOne) {
    EXPECT_EQ(runI32(
        "ServerModel zero #= ServerModel.sharedPool(0);\n"
        "if (zero.getPoolSize() != 1) return -1;\n"
        "ServerModel neg #= ServerModel.sharedPool(-4);\n"
        "if (neg.getPoolSize() != 1) return -2;\n"
        "if (!neg.isSharedPool()) return -3;\n"
        "return 1;"), 1);
}

// The two kind ordinals are the append-only contract the bind branch turns on.
TEST(ServerModelTests, kindOrdinalsAreDistinct) {
    EXPECT_EQ(runI32(
        "if (ServerModel.FIBER_PER_CONNECTION_KIND == ServerModel.SHARED_POOL_KIND) return -1;\n"
        "if (ServerModel.FIBER_PER_CONNECTION_KIND != 0) return -2;\n"
        "if (ServerModel.SHARED_POOL_KIND != 1) return -3;\n"
        "return 1;"), 1);
}
