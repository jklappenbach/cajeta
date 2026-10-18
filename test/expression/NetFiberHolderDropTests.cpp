// A scheduler regression guard over a loopback TcpStream. It lived in
// TlsLoopbackTest although it uses no TLS, and stayed here when that file's
// TLS cases moved to dev.cajeta.crypto (cajeta-crypto spec 6.3).

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.M");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

}  // namespace

// Scheduler regression guard (cross-carrier fiber pinning). A spawned fiber
// #-moves a TcpStream (whose ~TcpStream dtor runs on the fiber's stack) into a
// heap holder, then drops it at scope exit — no socket I/O. Before the
// home-carrier pinning fix this SIGSEGV'd deterministically on a >=4-carrier
// pool: a started fiber could be work-stolen and resumed on a different carrier
// than the one its ucontext was bound to, corrupting its stack mid-teardown.
// (Only reproduces when the pool has more carriers than fibers, i.e. >=4 cores;
// harmless but always-green elsewhere.)
TEST(NetFiberHolderDropTests, crossCarrierFiberHolderDropRegression) {
    std::string src =
        "package test;\n"
        "import cajeta.io.net.IpAddress;\n"
        "import cajeta.io.net.SocketAddress;\n"
        "import cajeta.io.net.TcpStream;\n"
        "import cajeta.io.net.TcpListener;\n"
        "import cajeta.concurrent.Tasks;\n"
        "public class Pipe {\n"
        "    TcpStream stream;\n"
        "    private Pipe(#TcpStream s) { this.stream #= s; }\n"
        "    public static #Pipe wrap(#TcpStream s) { return heap Pipe(#s); }\n"
        "}\n"
        "public final class M {\n"
        "    public static async int32 runServer(#TcpStream sock) {\n"
        "        Pipe p #= Pipe.wrap(#sock);\n"
        "        return 1;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        () -> int32 body = () -> {\n"
        "            IpAddress la #= IpAddress.loopbackV4();\n"
        "            SocketAddress bindAddr #= SocketAddress.of(#la, 0);\n"
        "            TcpListener listener #= TcpListener.bind(bindAddr);\n"
        "            int32 port = listener.boundPort();\n"
        "            IpAddress ca #= IpAddress.loopbackV4();\n"
        "            SocketAddress connAddr #= SocketAddress.of(#ca, port);\n"
        "            TcpStream client #= TcpStream.connect(#connAddr);\n"
        "            TcpStream server #= listener.accept();\n"
        "            Task<int32> t = spawn runServer(#server);\n"
        "            int32 sr = await t;\n"
        "            listener.close();\n"
        "            return sr;\n"
        "        };\n"
        "        return Tasks.runBlocking<int32>(body);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}
