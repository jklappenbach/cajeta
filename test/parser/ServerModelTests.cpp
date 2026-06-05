// NET-9.2 — cajeta.net.ServerModel selection + HttpServer model wiring.
//
// NET-9.2 is "run the existing handler-based HttpServer (NET-9.1) on BOTH
// NET-4 accept models — fiber-per-connection (Model A, NET-4.2) and the
// event-driven shared-pool (Model B, NET-4.3) — selected via the builder."
//
// The *selection* is a pure value decision: a ServerModel names which accept
// stack to materialize (and, for Model B, the worker-pool size), and the
// bind path branches on it (ServerModel.bindServer -> Server.bind for Model A
// vs SharedPoolServer.bind for Model B). That decision logic is socket-free
// and JIT-runnable deterministically, exactly the way the HttpServerTests
// pure byte path (NET-9.1) and the ServerModel value type are.
//
// The *live* accept-loop half (binding a real listener, accepting 500
// loopback clients on each model — HttpServerTests.bothModelsServeSameHandler)
// needs the in-scheduler reactor + loopback-socket harness the NET-4
// `ServerTests.*` JIT suite awaits, so it is out of scope here (mirrors
// HttpServerTests, which pins the protocol loop one level down rather than
// over a live accept loop). The handler-parity-across-models PROPERTY itself
// is pinned natively in test/net/HttpModelParityHarnessTests.cpp.
//
// Each test compiles a small Cajeta run() that builds a ServerModel (or an
// HttpServer builder), inspects the selection it encodes, and returns an
// int32 sentinel (1 on success, a distinct negative per failed sub-check).
//
// Pins NET-9.2: "Runs on both accept models: fiber-per-conn (NET-4.2) and
// shared-pool (NET-4.3), selected via the builder." (plan, Phase 9).

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

// Wrap a method body in a class importing the net model + http server types.
// The body must `return` int32.
int32_t runI32(const std::string& body) {
    std::string src =
        "package test;\n"
        "import cajeta.lang.String;\n"
        "import cajeta.net.ServerModel;\n"
        "import cajeta.net.http.HttpServer;\n"
        "import cajeta.net.http.HttpServerBuilder;\n"
        "import cajeta.net.http.HttpRequest;\n"
        "import cajeta.net.http.HttpResponse;\n"
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
        "ServerModel m = ServerModel.fiberPerConnection();\n"
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
        "ServerModel m = ServerModel.sharedPool(8);\n"
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
        "ServerModel zero = ServerModel.sharedPool(0);\n"
        "if (zero.getPoolSize() != 1) return -1;\n"
        "ServerModel neg = ServerModel.sharedPool(-4);\n"
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

// --- HttpServer builder threads the model through -----------------------

// HttpServer.builder().model(SharedPool(16)) records Model B on the builder's
// model so build() will materialize the shared-pool accept stack. We inspect
// the model the builder will use WITHOUT calling build() (which binds a live
// socket — out of scope for this pure-logic suite): the builder exposes its
// chosen model so the selection is testable socket-free.
TEST(ServerModelTests, builderModelThreadsSharedPool) {
    EXPECT_EQ(runI32(
        "HttpServerBuilder b = HttpServer.builder()\n"
        "    .bind(\"0.0.0.0:8080\")\n"
        "    .model(ServerModel.sharedPool(16))\n"
        "    .handler((req) -> HttpResponse.of(200));\n"
        "ServerModel m = b.selectedModel();\n"
        "if (!m.isSharedPool()) return -1;\n"
        "if (m.getPoolSize() != 16) return -2;\n"
        "return 1;"), 1);
}

// The .sharedPool(n) builder shorthand is equivalent to .model(sharedPool(n)).
TEST(ServerModelTests, builderSharedPoolShorthand) {
    EXPECT_EQ(runI32(
        "HttpServerBuilder b = HttpServer.builder()\n"
        "    .bind(\"0.0.0.0:8080\")\n"
        "    .sharedPool(4);\n"
        "ServerModel m = b.selectedModel();\n"
        "if (!m.isSharedPool()) return -1;\n"
        "if (m.getPoolSize() != 4) return -2;\n"
        "return 1;"), 1);
}

// The builder defaults to Model A (fiber-per-connection) when no model is set —
// the documented default.
TEST(ServerModelTests, builderDefaultsToFiberPerConnection) {
    EXPECT_EQ(runI32(
        "HttpServerBuilder b = HttpServer.builder().bind(\"0.0.0.0:8080\");\n"
        "ServerModel m = b.selectedModel();\n"
        "if (!m.isFiberPerConnection()) return -1;\n"
        "if (m.isSharedPool()) return -2;\n"
        "return 1;"), 1);
}

// A directly-built HttpServer (the pure-logic ctor) carries the default
// fiber-per-connection model, so serverModel() is well-defined even off the
// no-socket path.
TEST(ServerModelTests, directHttpServerDefaultsToFiberPerConnection) {
    EXPECT_EQ(runI32(
        "HttpServer srv = heap HttpServer((req) -> HttpResponse.of(200));\n"
        "ServerModel m = srv.serverModel();\n"
        "if (!m.isFiberPerConnection()) return -1;\n"
        "return 1;"), 1);
}
