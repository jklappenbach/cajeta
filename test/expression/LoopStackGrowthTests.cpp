// A construct inside a loop reuses one frame slot per site: a function body places
// every alloca in its entry block, so no iteration count can grow the stack.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

const char* PRE =
    "package test;\n"
    "import cajeta.io.net.SocketAddress;\n"
    "import cajeta.io.net.UdpSocket;\n"
    "import cajeta.io.net.RecvResult;\n"
    "public record Pt {\n"
    "    int32 x;\n"
    "    int32 y;\n"
    "}\n";

// The allocas `fn`'s body places outside its entry block, one line each.
std::string allocasOutsideEntry(const std::string& ir, const std::string& fn) {
    size_t at = ir.find("@\"" + fn);
    if (at == std::string::npos) return "<no function " + fn + ">";
    size_t start = ir.rfind("define ", at);
    size_t end = ir.find("\n}\n", at);
    std::string body = ir.substr(start, end - start);
    std::string out;
    bool inEntry = true;
    size_t pos = body.find('\n');
    while (pos != std::string::npos && pos < body.size()) {
        size_t next = body.find('\n', pos + 1);
        std::string line = body.substr(pos + 1, (next == std::string::npos ? body.size() : next) - pos - 1);
        if (!line.empty() && line[0] != ' ' && line.find(':') != std::string::npos
                && line.rfind("entry:", 0) != 0) {
            inEntry = false;
        }
        if (!inEntry && line.find(" = alloca ") != std::string::npos) out += line + "\n";
        pos = next;
    }
    return out;
}

} // namespace

TEST(LoopStackGrowthTests, loopBodiesAllocateNothing) {
    std::string src = std::string(PRE) +
        "public final class L {\n"
        "    public static int32 loops(UdpSocket u, int32[] xs, int32 n) {\n"
        "        Pt p = Pt { x: 1, y: 2 };\n"
        "        int8[] buf = heap int8[64];\n"
        "        int32 acc = 0;\n"
        "        int32 i = 0;\n"
        "        while (i < n) {\n"
        "            for (int32 v : xs) { acc = acc + v; }\n"
        "            Pt q = p.with(x: i);\n"
        "            acc = acc + q.x;\n"
        "            RecvResult r #= u.recvFrom(buf, 0, 64);\n"
        "            SocketAddress a #= u.localAddress();\n"
        "            System.stdout.println(\"i {}\", i);\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return acc;\n"
        "    }\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    CajetaJit::Options opts;
    opts.captureIr = true;
    auto jit = CajetaJit::compile(src, "test.L", opts);
    ASSERT_NE(jit, nullptr);
    EXPECT_EQ(allocasOutsideEntry(jit->getModuleIr(), "test.L::loops"), "");
}

TEST(LoopStackGrowthTests, recordWithAndNestedForeachRunLong) {
    std::string src = std::string(PRE) +
        "public final class L {\n"
        "    public static int32 run() {\n"
        "        Pt p = Pt { x: 1, y: 2 };\n"
        "        int32[] xs = heap int32[2];\n"
        "        xs[0] = 1; xs[1] = 2;\n"
        "        int64 acc = 0;\n"
        "        int32 i = 0;\n"
        "        while (i < 3000000) {\n"
        "            Pt q = p.with(x: i % 7);\n"
        "            for (int32 v : xs) { acc = acc + v; }\n"
        "            acc = acc + q.x;\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return (int32) (acc % 1000);\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.L");
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 994);
}
