// A generated mock is a @TestComponent for its target (primavera-web 0.2.6.2):
// built with --profile=test the DI graph injects it where the production
// component would go, and built for prod it injects the production one. A CLI
// build, because the JIT harness always compiles under the test profile.

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <string>
#include "../PortableEnv.h"

namespace fs = std::filesystem;

namespace {

std::string mockCompilerBinary() {
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

int mockExit(int status) {
#ifdef _WIN32
    return status;
#else
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

const std::map<std::string, std::string> kFiles = {
    {"dev/cajeta/unit/MockEngine.cajeta",
     "package dev.cajeta.unit;\n"
     "public class MockEngine {\n"
     "    public MockEngine() { }\n"
     "    public Object handle(String name, #Object[] a) { return \"stubbed\"; }\n"
     "    public #Object handleOwned(String name, #Object[] a) { return null; }\n"
     "}\n"},
    {"app/Directory.cajeta",
     "package app;\n"
     "@GenerateMock\n"
     "public interface Directory { String label(); }\n"},
    {"app/RealDirectory.cajeta",
     "package app;\n"
     "@Component\n"
     "public class RealDirectory implements Directory {\n"
     "    public RealDirectory() { }\n"
     "    public String label() { return \"real\"; }\n"
     "}\n"},
    {"app/Uses.cajeta",
     "package app;\n"
     "@Component\n"
     "public class Uses {\n"
     "    @Inject Directory d;\n"
     "    public Uses() { }\n"
     "}\n"},
    {"app/Main.cajeta",
     "package app;\n"
     "public class Main {\n"
     "    public static int32 main() {\n"
     "        Uses u = Uses.__cajeta_inject();\n"
     "        if (u.d.label().equals(\"stubbed\")) { return 1; }\n"
     "        if (u.d.label().equals(\"real\")) { return 2; }\n"
     "        return 3;\n"
     "    }\n"
     "}\n"},
};

// The program's exit code built under `profile` ("" for the default), or -100
// when the build failed; `output` gets the build log.
int buildAndRun(const std::string& profile, std::string& output) {
    static std::mt19937_64 rng(std::random_device{}());
    fs::path root = fs::temp_directory_path() / ("cajeta_mockprofile_" + std::to_string(rng()));
    for (const auto& [name, text] : kFiles) {
        fs::path p = root / "src" / name;
        fs::create_directories(p.parent_path());
        std::ofstream(p) << text;
    }
    fs::create_directories(root / "out");
    fs::path log = root / "build.log";
    fs::path exe = root / "out" / "prog";
    std::string flag = profile.empty() ? std::string() : " --profile=" + profile;
    std::string cmd = "\"" + mockCompilerBinary() + "\" --emit=exe" + flag + " -o \"" + exe.string()
        + "\" app.Main.main \"" + (root / "src").string() + "\" \"" + (root / "out").string()
        + "\" > \"" + log.string() + "\" 2>&1";
    int rc = mockExit(std::system(cajeta_shell(cmd).c_str()));
    {
        std::ifstream in(log);
        output.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    int result = -100;
    if (rc == 0) {
        std::string run = "\"" + cajeta_exe_path(exe).string() + "\" > " CAJETA_PORTABLE_DEVNULL " 2>&1";
        result = mockExit(std::system(cajeta_shell(run).c_str()));
    }
    std::error_code ec;
    fs::remove_all(root, ec);
    return result;
}

}  // namespace

TEST(MockProfileTests, aGeneratedMockStandsInForItsTargetUnderTheTestProfile) {
    std::string out;
    EXPECT_EQ(buildAndRun("test", out), 1) << "the test profile injects the generated mock:\n" << out;
}

TEST(MockProfileTests, theProductionComponentStaysOutsideTheTestProfile) {
    std::string out;
    EXPECT_EQ(buildAndRun("", out), 2) << "prod injects the production component:\n" << out;
}
