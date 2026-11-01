//
// An --emit=exe program runs the runtime's at-exit registry when its entry
// returns: @PreDestroy on a singleton and the cpu launch trace's dump ride it
// (libc atexit dangles in JIT'd runs). Cajeta.runAtExit() still fires the
// handlers early, and then they do not run again.
//

#include "gtest/gtest.h"

#include <array>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include "../PortableEnv.h"

namespace {

namespace fs = std::filesystem;

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
#ifdef _WIN32
    if (r.size() >= 3 && r[0] == '/' && std::isalpha((unsigned char) r[1]) && r[2] == '/')
        r = std::string(1, r[1]) + ":" + r.substr(2);
    std::string p = r + "/build/src/cajeta.exe";
    std::replace(p.begin(), p.end(), '/', '\\');
    return p;
#else
    return r + "/build/src/cajeta";
#endif
}

std::string capture(const std::string& cmd) {
    std::string out;
#ifdef _WIN32
    FILE* p = _popen((cmd + " 2>NUL").c_str(), "r");
#else
    FILE* p = popen((cmd + " 2>" CAJETA_PORTABLE_DEVNULL "").c_str(), "r");
#endif
    if (!p) return out;
    std::array<char, 512> buf;
    while (fgets(buf.data(), (int) buf.size(), p)) out += buf.data();
#ifdef _WIN32
    _pclose(p);
    out.erase(std::remove(out.begin(), out.end(), '\r'), out.end());
#else
    pclose(p);
#endif
    return out;
}

// Builds demo.Closer.main with `body` as main's statements; returns the output of a run.
std::string runExe(const std::string& tag, const std::string& body) {
    static std::mt19937_64 rng(std::random_device{}());
    fs::path base = fs::temp_directory_path() / ("cajeta_exeatexit_" + tag + "_" + std::to_string(rng()));
    fs::create_directories(base / "src" / "demo");
    fs::create_directories(base / "build");
    {
        std::ofstream out(base / "src" / "demo" / "Closer.cajeta");
        out << "package demo;\n"
            << "@Component public class Closer {\n"
            << "    public Closer() { return; }\n"
            << "    @PreDestroy\n"
            << "    public void close() { System.stdout.println(\"closed\"); return; }\n"
            << "    public static int32 main(String[] args) {\n"
            << "        Closer c = __cajeta_inject();\n"
            << body
            << "        System.stdout.println(\"returning\");\n"
            << "        return 0;\n"
            << "    }\n"
            << "}\n";
    }
    fs::path bin = base / "build" / "closer";
    std::string cmd = compilerBinary() + " --emit=exe -o " + bin.string() + " demo.Closer.main "
        + (base / "src").string() + " " + (base / "build").string() + " > " CAJETA_PORTABLE_DEVNULL " 2>&1";
    std::string out = "build failed";
    if (std::system(cmd.c_str()) == 0) {
        if (!fs::exists(bin)) bin += ".exe";
        if (fs::exists(bin)) out = capture(bin.string());
    }
    fs::remove_all(base);
    return out;
}

} // namespace

// FIRES: a singleton's @PreDestroy runs after the entry returns.
TEST(ExeAtExitTests, preDestroyRunsWhenTheEntryReturns) {
    EXPECT_EQ("returning\nclosed\n", runExe("ret", ""));
}

// DOES NOT FIRE twice: handlers fired early by Cajeta.runAtExit() are not run again at exit.
TEST(ExeAtExitTests, handlersFiredEarlyDoNotRunAgain) {
    EXPECT_EQ("closed\nreturning\n", runExe("early", "        Cajeta.runAtExit();\n"));
}
