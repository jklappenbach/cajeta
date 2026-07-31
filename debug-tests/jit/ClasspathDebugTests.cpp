//
// Debugging INTO a `--classpath` dependency.
//
// Dependency classes are compiled from the archive's ClassSource entries and
// spliced into the JIT's module list, so they carry safepoints and ARE
// steppable. What was broken is their IDENTITY: ingestClasspath built each
// external module through the synthetic ctor, which leaves `sourcePath` empty.
// assignDbgLocRanges keys on remappedSourcePath(), so every dependency module
// hashed to the same registry slot, took the same dbgLocBase, and restarted at
// dbgLocUsed 0 — overwriting the previous one's entries in the global loc
// table. Last writer won.
//
// That is why ONE dependency class resolved correctly and every other one's
// safepoints reported the winner's file and line, with breakpoints in them
// never matching (Julian, 2026-07-31, samples/tour: `Logger.cajeta` never
// fired; `Logger::info`, `Logger::logMsg` and `Levels::severity` all claimed
// `LogFmt.cajeta`). Two dependency classes in one archive is the minimum that
// reproduces it — testing only the winner passes against the bug.
//
#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

#include "../TempProgram.h"
#include "cajeta/dbg/DebugLocTable.h"
#include "cajeta/jit/CajetaJitHost.h"

namespace fs = std::filesystem;
using cajeta::debugtest::TempProgram;
using cajeta::jit::Breakpoint;
using cajeta::jit::JitRunOptions;
using cajeta::jit::startDebugSession;

namespace {

std::string compilerBinary() {
    const char* envRoot = std::getenv("CAJETA_SOURCE_ROOT");
    std::string r;
    if (envRoot && *envRoot) r = envRoot;
#ifdef CAJETA_SOURCE_ROOT_DEFAULT
    else r = CAJETA_SOURCE_ROOT_DEFAULT;
#else
    else r = ".";
#endif
    return r + "/build/src/cajeta";
}

const char* kDepOne =
    "package deplib;\n"
    "public class DepOne {\n"
    "    public static int32 first() {\n"
    "        int32 a = 1;\n"          // line 4
    "        return a + 39;\n"        // line 5
    "    }\n"
    "}\n";

const char* kDepTwo =
    "package deplib;\n"
    "public class DepTwo {\n"
    "    public static int32 second() {\n"
    "        int32 b = 1;\n"          // line 4
    "        return b + 1;\n"         // line 5
    "    }\n"
    "}\n";

// The consumer calls BOTH dependency classes, so both are reachable.
const char* kApp =
    "package app;\n"
    "import deplib.DepOne;\n"
    "import deplib.DepTwo;\n"
    "public class App {\n"
    "    public static int32 main() {\n"
    "        int32 x = DepOne.first();\n"
    "        int32 y = DepTwo.second();\n"
    "        return x + y;\n"
    "    }\n"
    "}\n";

// A consumer project plus a built `.cja` carrying DepOne + DepTwo.
struct DepProgram {
    TempProgram prog;
    fs::path lib;
    fs::path cja;   // "" when the archive build failed

    DepProgram() : prog("app", "App.cajeta", kApp) {
        static std::mt19937_64 rng(std::random_device{}());
        lib = fs::temp_directory_path()
            / ("cajeta_depdbg_" + std::to_string(rng()));
        fs::create_directories(lib / "src" / "deplib");
        { std::ofstream o(lib / "src" / "deplib" / "DepOne.cajeta"); o << kDepOne; }
        { std::ofstream o(lib / "src" / "deplib" / "DepTwo.cajeta"); o << kDepTwo; }
        fs::create_directories(lib / "arc");
        std::string cmd = compilerBinary() + " deplib.DepOne "
                        + (lib / "src").string() + " " + (lib / "arc").string()
                        + " --emit=cja > /dev/null 2>&1";
        if (std::system(cmd.c_str()) != 0) return;
        for (auto& e : fs::directory_iterator(lib / "arc"))
            if (e.path().extension() == ".cja") { cja = e.path(); break; }
    }
    ~DepProgram() {
        std::error_code ec;
        fs::remove_all(lib, ec);
    }

    JitRunOptions opts() const {
        JitRunOptions o;
        o.sourceRoot = prog.sourceRoot();
        o.entryMethod = "app.App.main";
        o.debugInfo = true;
        o.classpath = {cja.string()};
        return o;
    }
};

// A breakpoint that cannot bind does not fail the plain waitForStop() — the
// program runs to completion and nothing ever parks, so the wait blocks
// forever. Bound it, or the regression reports as a hung target.
constexpr std::chrono::milliseconds kStopTimeout{120000};

// Park on `line` of `file` and return the loc entry the stop resolved to.
void expectStopsIn(const DepProgram& p, const std::string& file, int line) {
    std::string err;
    auto session = startDebugSession(p.opts(), {Breakpoint{file, line}}, &err);
    ASSERT_NE(session, nullptr) << err;
    cajeta::dbg::StopEvent stop;
    ASSERT_TRUE(session->controller().waitForStop(stop, kStopTimeout))
        << "breakpoint in dependency source " << file << " never fired";
    const auto& loc = cajeta::dbg::globalDbgLocTable().at(stop.locId);
    EXPECT_EQ(fs::path(loc.file).filename().string(), file)
        << "dependency safepoint attributed to another dependency's file";
    EXPECT_EQ(loc.line, line);
    session->controller().resume();
    EXPECT_EQ(session->join(), 42);
}

#define SKIP_WITHOUT_BINARY() \
    if (!fs::exists(compilerBinary())) \
        GTEST_SKIP() << "compiler binary unavailable"

} // namespace

TEST(ClasspathDebug, BreakpointInDependencySourceStops) {
    SKIP_WITHOUT_BINARY();
    DepProgram p;
    ASSERT_FALSE(p.cja.empty()) << "dep .cja build failed";
    expectStopsIn(p, "DepOne.cajeta", 5);
}

// The second class in the SAME archive. Under the shared-range collision only
// one of the two could ever resolve, so this is the assertion that pins the
// per-module id ranges.
TEST(ClasspathDebug, ASecondDependencyClassInTheSameArchiveAlsoStops) {
    SKIP_WITHOUT_BINARY();
    DepProgram p;
    ASSERT_FALSE(p.cja.empty()) << "dep .cja build failed";
    expectStopsIn(p, "DepTwo.cajeta", 5);
}
