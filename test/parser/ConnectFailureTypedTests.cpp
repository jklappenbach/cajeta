//
// connectAsync failure typing (NET-3.3 / the cajeta-http 1.4c find).
//
// The connectAsync intrinsic's failure paths throw LEGACY INTEGER TAGS
// (IntToPtr'd small values). The catch dispatch matches a legacy tag to
// the FIRST clause unconditionally and binds it as the catch variable —
// so `catch (NetException e)` used to bind `e = (NetException*)0x107`,
// and the first caller to actually READ the caught object (`e.kind` in
// cajeta-http's retry policy) dereferenced the tag and SIGSEGV'd at
// 0x13f (= 0x107 + kind's offset).
//
// The fix: the intrinsic is renamed `connectAsyncNative` and now encodes
// the normalized `cajeta_net_err` ordinal into its tag (0x200 + err, the
// hard-fail path capturing last_error BEFORE close() clobbers errno);
// the public `TcpStream.connectAsync` is a cajeta wrapper that catches
// the tag and materializes the typed exception via `NetErrors.fromErrno`.
//
// This test pins the observable contract: a refused dial surfaces as a
// catchable NetException whose `kind` READS correctly as
// KIND_CONNECTION_REFUSED (2) — the read is the part that used to crash.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& dBody) {
    std::string src =
        std::string("package test;\n")
        + "import cajeta.io.net.NetException;\n"
        + "import cajeta.io.net.SocketAddress;\n"
        + "import cajeta.io.net.TcpListener;\n"
        + "import cajeta.io.net.TcpStream;\n"
        + "public final class D {\n" + dBody + "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

// A dial to a port nothing listens on must surface as a typed
// NetException whose kind reads KIND_CONNECTION_REFUSED — reading the
// caught object is the regression (it was a bound integer tag before).
TEST(ConnectFailureTypedTests, refusedDialReadsAsTypedException) {
    EXPECT_EQ(runI32(
        "    public static async int32 dialDead(int32 port) {\n"
        "        SocketAddress a = SocketAddress.parse(\"127.0.0.1:\" + port);\n"
        "        try {\n"
        "            TcpStream s = TcpStream.connectAsync(#a);\n"
        "            s.close();\n"
        "            return -1;\n"
        "        } catch (NetException e) {\n"
        "            return e.kind;\n"
        "        }\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        SocketAddress bindA = SocketAddress.parse(\"127.0.0.1:0\");\n"
        "        TcpListener lst = TcpListener.bind(bindA);\n"
        "        int32 port = lst.boundPort();\n"
        "        lst.close();\n"
        "        int32 kind = 0;\n"
        "        scope {\n"
        "            Task<int32> t = spawn dialDead(port);\n"
        "            kind = await t;\n"
        "        }\n"
        "        return kind;\n"
        "    }\n"), 2);   // NetException.KIND_CONNECTION_REFUSED
}

// The successful path must be untouched by the wrapper: dial a LIVE
// listener and complete an exchange-free connect/close round.
TEST(ConnectFailureTypedTests, liveDialStillConnects) {
    EXPECT_EQ(runI32(
        "    public static async int32 dialLive(int32 port) {\n"
        "        SocketAddress a = SocketAddress.parse(\"127.0.0.1:\" + port);\n"
        "        try {\n"
        "            TcpStream s = TcpStream.connectAsync(#a);\n"
        "            s.close();\n"
        "            return 7;\n"
        "        } catch (NetException e) {\n"
        "            return -1;\n"
        "        }\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        SocketAddress bindA = SocketAddress.parse(\"127.0.0.1:0\");\n"
        "        TcpListener lst = TcpListener.bind(bindA);\n"
        "        int32 port = lst.boundPort();\n"
        "        int32 got = 0;\n"
        "        scope {\n"
        "            Task<int32> t = spawn dialLive(port);\n"
        "            got = await t;\n"
        "        }\n"
        "        lst.close();\n"
        "        return got;\n"
        "    }\n"), 7);
}
