// Every test TU that calls setenv/unsetenv must obtain them portably.
//
// POSIX setenv(3)/unsetenv(3) do not exist in the MinGW/MSVCRT C library, so
// a TU that calls them without `PortableEnv.h` (or its own _putenv_s shim)
// compiles everywhere EXCEPT Windows. That makes it invisible to a full local
// sweep — the failure surfaces only in a CI dry-run, where ninja stops at the
// first offending TU and hides any others behind it. That is exactly what
// happened on 2026-08-27: the dry-run reported VectorDotAccumTests.cpp, and a
// second file (XpuCpuWaveSimdTests.cpp) was sitting behind it with the same
// defect, which would have cost another full dry-run cycle to discover.
//
// This test is the check moved off a human's memory and into the suite, where
// it runs on every platform in every sweep. It is a source scan rather than a
// compile check because the whole point is to catch it on a host where the
// code compiles fine.

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

fs::path testSourceRoot() {
    const char* env = std::getenv("CAJETA_SOURCE_ROOT");
    std::string r;
    if (env && *env) r = env;
    else {
#ifdef CAJETA_SOURCE_ROOT_DEFAULT
        r = CAJETA_SOURCE_ROOT_DEFAULT;
#else
        r = ".";
#endif
    }
    return fs::path(r) / "test";
}

}  // namespace

TEST(PortableEnvGuardTests, everyCallerOfSetenvObtainsItPortably) {
    fs::path root = testSourceRoot();
    ASSERT_TRUE(fs::is_directory(root))
        << "cannot locate the test sources at " << root
        << " — set CAJETA_SOURCE_ROOT";

    // A call, not a mention: `setenv(` or `::setenv(` not preceded by an
    // identifier character, so `_putenv_s` and the shim's own definitions in
    // PortableEnv.h do not count as calls.
    const std::regex callRe(R"((^|[^_A-Za-z0-9:])(::)?(un)?setenv[[:space:]]*\()");

    std::vector<std::string> offenders;
    for (const auto& e : fs::recursive_directory_iterator(root)) {
        if (!e.is_regular_file()) continue;
        std::string ext = e.path().extension().string();
        if (ext != ".cpp" && ext != ".h") continue;
        if (e.path().filename() == "PortableEnv.h") continue;

        std::ifstream in(e.path());
        std::stringstream ss;
        ss << in.rdbuf();
        const std::string src = ss.str();

        if (!std::regex_search(src, callRe)) continue;
        // Protected either by the shared header or by a local _putenv_s shim.
        if (src.find("PortableEnv.h") != std::string::npos) continue;
        if (src.find("_putenv_s") != std::string::npos) continue;
        // Or by being excluded from the Windows build outright.
        if (src.find("#if !defined(_WIN32)") != std::string::npos ||
            src.find("#ifndef _WIN32") != std::string::npos) continue;

        offenders.push_back(fs::relative(e.path(), root).string());
    }

    std::string joined;
    for (const auto& o : offenders) joined += "\n  " + o;
    EXPECT_TRUE(offenders.empty())
        << "these test TUs call setenv/unsetenv with no portable source. They"
           " compile here and FAIL on MinGW:" << joined
        << "\n\nFix: #include \"../PortableEnv.h\" (adjust the depth).";
}

// A host-side symbol that JIT'd code must reach needs a Windows bridge entry.
//
// Same family, different mechanism. `__attribute__((visibility("default")))`
// is how a host symbol is published to the JIT's process-symbol generator on
// ELF — and it is a NO-OP on COFF, where a PE exports nothing regardless. So
// a symbol marked that way and not also listed in CajetaJitWinSymbols.cpp
// resolves on Linux and macOS and is invisible on Windows.
//
// Measured 2026-08-28: the three __cajeta_install_* symbols behind
// Packages.install were marked visibility("default") and never bridged. They
// are referenced from cajeta_rt_session.c, which is part of the STANDARD
// embedded runtime, so the unresolved reference poisoned the runtime module
// for every JIT'd cell — 82 failures across suites as unrelated as Protobuf,
// Avro, Vmap and Varargs, from one missing table entry. This scan is the
// cheapest possible statement of that invariant, and it runs on any host.
TEST(PortableEnvGuardTests, hostSymbolsPublishedToTheJitAreBridgedForWindows) {
    fs::path src = testSourceRoot().parent_path() / "src";
    ASSERT_TRUE(fs::is_directory(src)) << "cannot locate src at " << src;

    const fs::path bridge = src / "cajeta" / "jit" / "CajetaJitWinSymbols.cpp";
    ASSERT_TRUE(fs::exists(bridge))
        << "the Windows symbol bridge has moved — update this test: " << bridge;
    std::string bridgeSrc;
    {
        std::ifstream in(bridge);
        std::stringstream ss;
        ss << in.rdbuf();
        bridgeSrc = ss.str();
    }

    const std::regex identRe(R"(\b(__cajeta_[A-Za-z0-9_]+))");
    std::vector<std::string> unbridged;

    for (const auto& e : fs::recursive_directory_iterator(src)) {
        if (!e.is_regular_file()) continue;
        std::string ext = e.path().extension().string();
        if (ext != ".cpp" && ext != ".h") continue;
        if (e.path() == bridge) continue;

        std::ifstream in(e.path());
        std::string line;
        bool armed = false;
        while (std::getline(in, line)) {
            if (line.find("visibility(\"default\")") != std::string::npos) {
                armed = true;
                continue;
            }
            if (!armed) continue;
            // The declaration follows the attribute; take the first
            // __cajeta_* identifier on it. Blank lines don't disarm.
            std::smatch m;
            if (std::regex_search(line, m, identRe)) {
                const std::string sym = m[1].str();
                if (bridgeSrc.find("\"" + sym + "\"") == std::string::npos) {
                    unbridged.push_back(
                        sym + "  (" + fs::relative(e.path(), src).string() + ")");
                }
                armed = false;
            } else if (!line.empty()) {
                armed = false;
            }
        }
    }

    std::string joined;
    for (const auto& u : unbridged) joined += "\n  " + u;
    EXPECT_TRUE(unbridged.empty())
        << "these host symbols are published to the JIT via visibility"
           "(\"default\"), which does nothing on COFF, and are NOT in the"
           " Windows bridge. They resolve here and fail on MinGW:" << joined
        << "\n\nFix: add a CJ_SYM entry in src/cajeta/jit/CajetaJitWinSymbols"
           ".cpp (and test/jit/JitWinSymbols.c if tests JIT them too).";
}

// The negative arm: the guard must be capable of firing. A scanner whose
// pattern silently matched nothing would report a clean run forever, which is
// the failure mode that makes checks like this worthless.
TEST(PortableEnvGuardTests, theScanActuallyMatchesACall) {
    const std::regex callRe(R"((^|[^_A-Za-z0-9:])(::)?(un)?setenv[[:space:]]*\()");

    EXPECT_TRUE(std::regex_search(std::string("    setenv(\"A\", \"1\", 1);"), callRe));
    EXPECT_TRUE(std::regex_search(std::string("    ::unsetenv(\"A\");"), callRe));
    // And must not fire on the things that are not calls.
    EXPECT_FALSE(std::regex_search(std::string("  return _putenv_s(k, v);"), callRe));
    EXPECT_FALSE(std::regex_search(std::string("  const char* v = getenv(\"A\");"), callRe));
}
