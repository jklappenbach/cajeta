// A shutdown() that lands before serve() pre-empts it. It used to leave the
// server NEW, so a later serve() started accepting and never returned.

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

// 1 when serve() returns at once after an early shutdown, 2 when it had to be
// shut down a second time to end.
std::string source(const std::string& model) {
    return
        "package test;\n"
        "import cajeta.concurrent.Fiber;\n"
        "import cajeta.io.net.Server;\n"
        "import cajeta.io.net.ServerBuilder;\n"
        "import cajeta.io.net.ServerModel;\n"
        "import cajeta.io.net.TcpStream;\n"
        "import cajeta.concurrent.Tasks;\n"
        "import cajeta.time.Duration;\n"
        "public final class M {\n"
        "    static boolean returned;\n"
        "    public static async int32 serveIt(Server s) {\n"
        "        s.serve();\n"
        "        M.returned = true;\n"
        "        return 1;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        () -> int32 body = () -> {\n"
        "            (TcpStream) -> void h = (TcpStream c) -> { };\n"
        "            ServerBuilder b = heap ServerBuilder();\n"
        "            b.bind(\"127.0.0.1:0\");\n"
        "            b.model(" + model + ");\n"
        "            b.handler(h);\n"
        "            Server s #= b.build();\n"
        "            s.shutdown(Duration.ofMillis(0L));\n"
        "            M.returned = false;\n"
        "            boolean early = false;\n"
        "            scope {\n"
        "                Task<int32> t = spawn serveIt(s);\n"
        "                Fiber.sleep(Duration.ofMillis(300L));\n"
        "                early = M.returned;\n"
        "                if (!early) {\n"
        "                    s.shutdown(Duration.ofMillis(0L));\n"
        "                }\n"
        "                int32 r = await t;\n"
        "            }\n"
        "            return early ? 1 : 2;\n"
        "        };\n"
        "        return Tasks.runBlocking<int32>(body);\n"
        "    }\n"
        "}\n";
}

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.M");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

TEST(ServerShutdownBeforeServeTests, fiberPerConnectionServeReturns) {
    EXPECT_EQ(runI32(source("ServerModel.fiberPerConnection()")), 1);
}

TEST(ServerShutdownBeforeServeTests, sharedPoolServeReturns) {
    EXPECT_EQ(runI32(source("ServerModel.sharedPool(2)")), 1);
}
