// An --emit=exe executable must LINK, whatever the debug-info setting.
//
// The embedded stdlib runtime (runtime/native/cajeta_rt_session.c) declares
// three host globals `extern` — __cajeta_install_hook / _ctx / _out — which
// back cajeta.session.Packages.install. That is correct for the JIT: the
// definitions live in the compiler host (KernelSession.cpp) and a JIT session
// binds them through its process-symbol generator, so cell code and the host
// address the SAME object. It is exactly wrong for AOT: an --emit=exe binary
// has no host and no generator, so `__cajeta_session_install` drags three
// undefined symbols into the link.
//
// Measured 2026-08-28: `cajeta build` could not link ANY executable project,
// because the build tool passes --debug-info=full (the debug flavor's
// default) and that keeps __cajeta_session_install alive:
//
//     ld.lld: error: undefined symbol: __cajeta_install_ctx
//     >>> referenced by cajeta.runtime.__stdlib__
//
// It was invisible because whether the link succeeds depends on whether DCE
// happens to drop that one function: --debug-info=line (the default) links
// fine and `full` does not. A program that links by luck is the thing this
// test removes — so both ends of the range are asserted, not just the one
// that was reported.
//
// The fix mirrors the OptiX and TLS precedent in Compiler.cpp — a generated
// weak stub linked into every exe. Weak, so the host's strong definitions
// still win wherever there is a host. With the stub the hook is null, which
// __cajeta_session_install already handles by reporting "no live session" —
// precisely the truth for an AOT binary.

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include "../PortableEnv.h"

namespace fs = std::filesystem;

namespace {

std::string aotCompilerBinary() {
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

int aotExit(int status) {
#ifdef _WIN32
    return status;
#else
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

struct AotWorld {
    fs::path root;
    AotWorld() {
        static std::mt19937_64 rng(std::random_device{}());
        root = fs::temp_directory_path()
             / ("cajeta_aotsym_" + std::to_string(rng()));
        fs::create_directories(root / "src" / "app");
        fs::create_directories(root / "out");
        std::ofstream(root / "src" / "app" / "Main.cajeta")
            << "package app;\n"
               "import cajeta.lang.String;\n"
               "import cajeta.lang.System;\n"
               "public class Main {\n"
               "    public static int32 main(String[] args) {\n"
               "        System.stdout.println(\"ok\");\n"
               "        return 0;\n"
               "    }\n"
               "}\n";
    }
    ~AotWorld() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    // Returns the exit code; `output` gets the combined log.
    int build(const std::string& extraFlags, std::string& output) const {
        fs::path out = root / "out";
        std::error_code ec;
        fs::remove_all(out, ec);
        fs::create_directories(out, ec);
        fs::path log = root / "build.log";
        std::string cmd = "\"" + aotCompilerBinary() + "\" --emit=exe "
            + extraFlags + " -o \"" + (out / "prog").string() + "\" "
            + "app.Main.main \"" + (root / "src").string() + "\" \""
            + out.string() + "\" > \"" + log.string() + "\" 2>&1";
        int rc = aotExit(std::system(cajeta_shell(cmd).c_str()));
        std::ifstream in(log);
        output.assign(std::istreambuf_iterator<char>(in),
                      std::istreambuf_iterator<char>());
        return rc;
    }

    bool producedExe() const { return fs::exists(root / "out" / "prog"); }
};

}  // namespace

// The regression itself: the flag set `cajeta build` actually passes.
TEST(AotSessionSymbolLinkTests, debugInfoFullExecutableLinks) {
    AotWorld w;
    std::string out;
    int rc = w.build("--mode=debug --opt=O0 --lto=off --debug-info=full "
                     "--bounds=on", out);
    EXPECT_EQ(0, rc) << "the debug flavor's own flag set must link:\n" << out;
    EXPECT_TRUE(w.producedExe());
    EXPECT_EQ(std::string::npos, out.find("undefined symbol"))
        << "no symbol may be left for the AOT linker to guess at:\n" << out;
}

// The other end of the range. `off` is the setting that keeps the LEAST
// alive, so if the reference survives here it is not debug records holding
// it — it is that the symbol has no definition to reach at all.
TEST(AotSessionSymbolLinkTests, debugInfoOffExecutableLinks) {
    AotWorld w;
    std::string out;
    int rc = w.build("--debug-info=off", out);
    EXPECT_EQ(0, rc) << "--debug-info=off must link:\n" << out;
    EXPECT_TRUE(w.producedExe());
    EXPECT_EQ(std::string::npos, out.find("undefined symbol")) << out;
}

// The settings that happened to link before must keep linking — this is the
// arm that would catch a "fix" that traded one broken configuration for
// another.
TEST(AotSessionSymbolLinkTests, theConfigurationsThatAlreadyLinkedStillDo) {
    for (const char* flags : {"", "--debug-info=line", "--mode=release"}) {
        AotWorld w;
        std::string out;
        int rc = w.build(flags, out);
        EXPECT_EQ(0, rc) << "regressed for flags [" << flags << "]:\n" << out;
        EXPECT_TRUE(w.producedExe()) << "no exe for flags [" << flags << "]";
    }
}
