//
// The --emit=exe shim bridges crt0's `main(argc, argv)` to a user
// `static int32 main(String[] args)`. C argv[0] is the PROGRAM NAME, not a
// user argument: Java-style args exclude it, and the JIT host already passes
// program args only. The shim originally forwarded the full argc/argv, so an
// AOT binary saw its own path as args[0] while the same program under the JIT
// saw the first real argument — cajeta-llama's CLI verb dispatch
// (`args[0].equals("run")`) worked under the JIT and bounced to usage as an
// exe. These tests pin the exe-side contract.
//

#include "gtest/gtest.h"

#include <array>
#include <algorithm>   // std::replace, used only in the _WIN32 branch below
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

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

fs::path freshTempDir() {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = fs::temp_directory_path()
              / ("cajeta_exeargs_" + std::to_string(rng()));
    fs::create_directories(base / "src" / "demo");
    fs::create_directories(base / "build");
    return base;
}

std::string capture(const std::string& cmd) {
    std::string out;
    FILE* p = popen((cmd + " 2>/dev/null").c_str(), "r");
    if (!p) return out;
    std::array<char, 512> buf;
    while (fgets(buf.data(), (int) buf.size(), p)) out += buf.data();
    pclose(p);
    return out;
}

// One shared fixture: build an exe whose main() echoes its args, one per
// line, prefixed by the count.
struct EchoExe {
    fs::path base;
    fs::path bin;
    bool ok = false;
};

EchoExe buildEchoExe() {
    EchoExe e;
    e.base = freshTempDir();
    {
        std::ofstream out(e.base / "src" / "demo" / "Echo.cajeta");
        out << "package demo;\n"
            << "public final class Echo {\n"
            << "    public static int32 main(String[] args) {\n"
            << "        System.stdout.println((int64) args.count());\n"
            << "        int32 i = 0;\n"
            << "        while (i < (int32) args.count()) {\n"
            << "            System.stdout.println(args[i]);\n"
            << "            i = i + 1;\n"
            << "        }\n"
            << "        return 0;\n"
            << "    }\n"
            << "}\n";
    }
    std::string cmd = compilerBinary() + " --emit=exe -o "
        + (e.base / "build" / "echo").string() + " demo.Echo.main "
        + (e.base / "src").string() + " " + (e.base / "build").string()
        + " > /dev/null 2>&1";
    e.bin = e.base / "build" / "echo";
    e.ok = std::system(cmd.c_str()) == 0 && fs::exists(e.bin);
    return e;
}

} // namespace

// args[] holds ONLY the user arguments: argv[0] (the binary's own path) is
// sliced off, so a verb dispatcher sees the verb at args[0] exactly as it
// does under the JIT.
TEST(ExeMainArgsTests, argsExcludeTheProgramName) {
    auto e = buildEchoExe();
    ASSERT_TRUE(e.ok) << "exe build failed";
    auto out = capture(e.bin.string() + " run --model m.gguf");
    EXPECT_EQ("3\nrun\n--model\nm.gguf\n", out);
    fs::remove_all(e.base);
}

// No user arguments → an EMPTY args array, not a 1-element array holding the
// program path, and not a negative-count array (the argc-1 clamp).
TEST(ExeMainArgsTests, noArgsYieldsEmptyArray) {
    auto e = buildEchoExe();
    ASSERT_TRUE(e.ok) << "exe build failed";
    auto out = capture(e.bin.string());
    EXPECT_EQ("0\n", out);
    fs::remove_all(e.base);
}
