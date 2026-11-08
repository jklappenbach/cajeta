// Thin LTO links with an ld.lld of this compiler's LLVM, found beside the compiler or
// in its configured tools bin before PATH: on a ROCm box PATH's ld.lld is another LLVM's
// and rejected the fork's summary version ("Invalid summary version 13"), failing every
// release build that did not put the fork's bin first (lld-lookup-path-only, 2026-11-08).

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include "../PortableEnv.h"
#include "cajeta/compile/LinkTools.h"

namespace fs = std::filesystem;

TEST(LldLookupTests, theMajorVersionIsReadFromAnLldVersionLine) {
    EXPECT_EQ(23, cajeta::lldMajorOf("LLD 23.0.0 (git@github.com:jklappenbach/cajeta-llvm.git b0be633) (compatible with GNU linkers)"));
    EXPECT_EQ(22, cajeta::lldMajorOf("AMD LLD 22.0.0 (https://github.com/ROCm/llvm-project.git 08a72fc+PATCHED) (compatible with GNU linkers)"));
    EXPECT_EQ(0, cajeta::lldMajorOf("GNU ld (GNU Binutils for Ubuntu) 2.42"));
    EXPECT_EQ(0, cajeta::lldMajorOf(""));
    EXPECT_EQ(0, cajeta::lldMajorOf("LLD x"));
}

namespace {
std::string lldCompilerBinary() {
    const char* envRoot = std::getenv("CAJETA_SOURCE_ROOT");
    std::string r = (envRoot && *envRoot) ? envRoot :
#ifdef CAJETA_SOURCE_ROOT_DEFAULT
        CAJETA_SOURCE_ROOT_DEFAULT;
#else
        ".";
#endif
    return r + "/build/src/cajeta";
}
}  // namespace

// A thin-LTO executable either links through a version-matched lld or says by name that
// none was found and builds without LTO; it never hands the bitcode to a foreign lld.
TEST(LldLookupTests, aThinLtoBuildNeverLinksThroughAForeignLld) {
    static std::mt19937_64 rng(std::random_device{}());
    fs::path root = fs::temp_directory_path() / ("cajeta_lld_" + std::to_string(rng()));
    fs::create_directories(root / "src" / "app");
    fs::create_directories(root / "out");
    std::ofstream(root / "src" / "app" / "Main.cajeta")
        << "package app;\n"
           "import cajeta.lang.String;\n"
           "import cajeta.lang.System;\n"
           "public class Main {\n"
           "    public static int32 main(String[] args) {\n"
           "        System.stdout.println(\"lto ok\");\n"
           "        return 0;\n"
           "    }\n"
           "}\n";
    fs::path log = root / "build.log";
    fs::path exe = root / "out" / "prog";
    std::string cmd = "\"" + lldCompilerBinary() + "\" --emit=exe --release --lto=thin -o \""
        + exe.string() + "\" app.Main.main \"" + (root / "src").string() + "\" \""
        + (root / "out").string() + "\" > \"" + log.string() + "\" 2>&1";
    int rc = std::system(cajeta_shell(cmd).c_str());
    std::string out;
    {
        std::ifstream in(log);
        out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    EXPECT_EQ(0, rc) << out;
    EXPECT_EQ(std::string::npos, out.find("Invalid summary version")) << out;
    if (out.find("needs lld") != std::string::npos) {
        EXPECT_NE(std::string::npos, out.find("Looked at:")) << out;
    }
    std::error_code ec;
    fs::remove_all(root, ec);
}
