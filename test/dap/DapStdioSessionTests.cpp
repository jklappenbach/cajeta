// `cajeta dap` end-to-end over real pipes.
//
// DapServerTests/DapInspectionTests drive handle() in-process, which never
// touches runOverStdio() — the serve loop, the Content-Length framing on the
// wire, the stdout PUMP (the debuggee's own prints become DAP `output`
// events), and the process lifecycle (disconnect ends the SESSION; the
// process ends at stdin EOF). This spawns the real binary and speaks the
// protocol to it, so those paths — and the JIT launch path underneath —
// run as an IDE actually drives them.

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::string compilerBinary() {
    const char* envRoot = std::getenv("CAJETA_SOURCE_ROOT");
    std::string r;
    if (envRoot && *envRoot) r = envRoot;
    else {
#ifdef CAJETA_SOURCE_ROOT_DEFAULT
        r = CAJETA_SOURCE_ROOT_DEFAULT;
#else
        r = ".";
#endif
    }
    return r + "/build/src/cajeta";
}

int exitCodeOf(int systemStatus) {
#ifdef _WIN32
    return systemStatus;
#else
    return WIFEXITED(systemStatus) ? WEXITSTATUS(systemStatus) : -1;
#endif
}

// One DAP request as a Content-Length frame.
std::string frame(const std::string& body) {
    return "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}

struct DapWorld {
    fs::path root;
    DapWorld() {
        static std::mt19937_64 rng(std::random_device{}());
        root = fs::temp_directory_path()
             / ("cajeta_dapio_" + std::to_string(rng()));
        fs::create_directories(root / "test");
    }
    ~DapWorld() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    fs::path program() const { return root / "test" / "D.cajeta"; }
    fs::path scriptIn() const { return root / "session.dap"; }
    fs::path outLog() const { return root / "session.out"; }

    void writeProgram(const std::string& src) const {
        std::ofstream(program()) << src;
    }

    // Feed a whole scripted session on stdin; the server processes frames
    // until EOF, which is also how the IDE plugin ends the process.
    int drive(const std::vector<std::string>& requests) const {
        std::ofstream in(scriptIn(), std::ios::binary);
        for (const auto& r : requests) in << frame(r);
        in.close();
        std::string cmd = compilerBinary() + " dap < " + scriptIn().string()
            + " > " + outLog().string() + " 2>/dev/null";
        return exitCodeOf(std::system(cmd.c_str()));
    }

    std::string output() const {
        std::ifstream in(outLog(), std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    // How many Content-Length frames came back.
    static size_t frameCount(const std::string& s) {
        size_t n = 0, pos = 0;
        while ((pos = s.find("Content-Length:", pos)) != std::string::npos) {
            ++n;
            pos += 15;
        }
        return n;
    }
};

std::string req(int seq, const std::string& command,
                const std::string& argsJson = "{}") {
    return "{\"seq\":" + std::to_string(seq) + ",\"type\":\"request\""
         + ",\"command\":\"" + command + "\""
         + ",\"arguments\":" + argsJson + "}";
}

} // namespace

// initialize over the wire: the response and the `initialized` event come
// back as well-formed Content-Length frames, and stdin EOF ends the process
// cleanly (exit 0).
TEST(DapStdioSessionTests, initializeOverTheWireThenEofExits) {
    DapWorld w;
    EXPECT_EQ(w.drive({req(1, "initialize")}), 0) << w.output();
    std::string out = w.output();
    EXPECT_GE(DapWorld::frameCount(out), 2u) << out;
    EXPECT_NE(out.find("\"command\":\"initialize\""), std::string::npos)
        << out;
    EXPECT_NE(out.find("\"event\":\"initialized\""), std::string::npos)
        << out;
    EXPECT_NE(out.find("supportsConfigurationDoneRequest"), std::string::npos)
        << out;
}

// A full scripted session: launch → breakpoint → stop → stackTrace →
// continue → exited/terminated, all over the wire. The program's own
// print becomes an `output` event through the stdout pump.
TEST(DapStdioSessionTests, scriptedSessionStopsResumesAndPumpsProgramOutput) {
    DapWorld w;
    w.writeProgram(
        "package test;\n"                                     // 1
        "import cajeta.lang.System;\n"                        // 2
        "public final class D {\n"                            // 3
        "    public static int32 run() {\n"                   // 4
        "        int32 x = 20;\n"                             // 5  <- bp
        "        System.stdout.println(\"from-the-debuggee\");\n" // 6
        "        return x + 1;\n"                             // 7
        "    }\n"
        "}\n");

    std::string launchArgs =
        "{\"entry-method\":\"test.D.run\",\"sourceRoot\":\""
        + w.root.string() + "\",\"stopOnEntry\":false}";
    std::string bpArgs =
        "{\"source\":{\"path\":\"" + w.program().string()
        + "\"},\"breakpoints\":[{\"line\":5}]}";

    EXPECT_EQ(w.drive({
        req(1, "initialize"),
        req(2, "launch", launchArgs),
        req(3, "setBreakpoints", bpArgs),
        req(4, "configurationDone"),
        req(5, "threads"),
        req(6, "stackTrace", "{\"threadId\":0}"),
        req(7, "continue", "{\"threadId\":0}"),
        req(8, "disconnect"),
    }), 0) << w.output();

    std::string out = w.output();
    EXPECT_NE(out.find("\"event\":\"stopped\""), std::string::npos) << out;
    EXPECT_NE(out.find("\"reason\":\"breakpoint\""), std::string::npos)
        << out;
    EXPECT_NE(out.find("D.cajeta"), std::string::npos) << out;
    // The pump turns the debuggee's stdout into a DAP output event rather
    // than corrupting the protocol stream.
    EXPECT_NE(out.find("\"event\":\"output\""), std::string::npos) << out;
    EXPECT_NE(out.find("from-the-debuggee"), std::string::npos) << out;
    EXPECT_NE(out.find("\"event\":\"exited\""), std::string::npos) << out;
    EXPECT_NE(out.find("\"event\":\"terminated\""), std::string::npos) << out;
    // Every emitted frame is well-formed: the pump never interleaves raw
    // program bytes into the protocol channel.
    EXPECT_EQ(out.find("from-the-debuggee\nContent-Length"), std::string::npos)
        << out;
}

// disconnect ends the SESSION, not the process (resident lifecycle): a
// second initialize on the same connection is served normally.
TEST(DapStdioSessionTests, disconnectEndsSessionAndTheServerKeepsServing) {
    DapWorld w;
    EXPECT_EQ(w.drive({
        req(1, "initialize"),
        req(2, "disconnect"),
        req(3, "initialize"),
        req(4, "threads"),
    }), 0) << w.output();

    std::string out = w.output();
    // Two initialize responses on one process.
    size_t first = out.find("\"command\":\"initialize\"");
    ASSERT_NE(first, std::string::npos) << out;
    EXPECT_NE(out.find("\"command\":\"initialize\"", first + 1),
              std::string::npos) << out;
    EXPECT_NE(out.find("\"command\":\"threads\""), std::string::npos) << out;
}

// Garbage on the wire must not wedge or crash the server: a frame whose
// body isn't JSON is refused, and the process still exits cleanly at EOF.
TEST(DapStdioSessionTests, malformedFrameDoesNotWedgeTheServer) {
    DapWorld w;
    std::ofstream in(w.scriptIn(), std::ios::binary);
    in << frame("{\"seq\":1,\"type\":\"request\",\"command\":\"initialize\","
                "\"arguments\":{}}");
    in << "Content-Length: 7\r\n\r\nNOTJSON";
    in.close();

    std::string cmd = compilerBinary() + " dap < " + w.scriptIn().string()
        + " > " + w.outLog().string() + " 2>/dev/null";
    EXPECT_EQ(exitCodeOf(std::system(cmd.c_str())), 0) << w.output();
    EXPECT_NE(w.output().find("\"command\":\"initialize\""),
              std::string::npos) << w.output();
}

// An unknown command gets a failed response rather than silence, so the
// client isn't left waiting on a request the server dropped.
TEST(DapStdioSessionTests, unknownCommandAnswersWithFailure) {
    DapWorld w;
    EXPECT_EQ(w.drive({req(1, "initialize"), req(2, "frobnicate")}), 0)
        << w.output();
    std::string out = w.output();
    size_t at = out.find("\"command\":\"frobnicate\"");
    ASSERT_NE(at, std::string::npos) << out;
    // The response for it carries success:false.
    size_t win = out.rfind("{", at);
    EXPECT_NE(out.find("\"success\":false", win), std::string::npos) << out;
}
