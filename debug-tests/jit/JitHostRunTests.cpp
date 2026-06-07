//
// End-to-end tests for the in-process JIT host (CP1 of the debugger plan).
// Each test writes a tiny Cajeta program to a temp source root, then drives
// cajeta::jit::runJit — exercising the full parse → codegen → merge → LLJIT →
// invoke pipeline inside this process. The contract under test is runJit's
// return code: an int32 entry's value flows back as the exit code, and the
// error paths return their documented non-zero codes.
//
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

#include "cajeta/jit/CajetaJitHost.h"

namespace fs = std::filesystem;
using cajeta::jit::JitRunOptions;
using cajeta::jit::runJit;

namespace {

// Write `source` at <tempRoot>/<pkgRelPath>/<fileName> and return tempRoot
// (the source root to hand runJit). The package-derived path must match the
// program's `package` declaration, per the normal compiler convention.
fs::path writeProgram(const std::string& pkgRelPath,
                      const std::string& fileName,
                      const std::string& source) {
    static std::mt19937_64 rng(std::random_device{}());
    fs::path root = fs::temp_directory_path()
                  / ("cajeta_dbgtest_" + std::to_string(rng()));
    fs::path dir = root / pkgRelPath;
    fs::create_directories(dir);
    std::ofstream out(dir / fileName);
    out << source;
    out.close();
    return root;
}

struct TempProgram {
    fs::path root;
    ~TempProgram() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
};

} // namespace

// A constant-returning int32 entry: the simplest possible program. Proves the
// host compiles, JITs, runs, and surfaces the return value.
TEST(JitHost, RunsConstantIntEntry) {
    TempProgram p{writeProgram("demo", "Prog.cajeta",
        "package demo;\n"
        "public class Prog {\n"
        "    public static int32 main() {\n"
        "        return 42;\n"
        "    }\n"
        "}\n")};
    JitRunOptions opts;
    opts.sourceRoot = p.root.string();
    opts.entryMethod = "demo.Prog.main";
    EXPECT_EQ(runJit(opts), 42);
}

// Locals + arithmetic — exercises a little real codegen (allocas, a multiply)
// rather than a folded constant.
TEST(JitHost, RunsArithmeticWithLocals) {
    TempProgram p{writeProgram("demo", "Calc.cajeta",
        "package demo;\n"
        "public class Calc {\n"
        "    public static int32 main() {\n"
        "        int32 a = 6;\n"
        "        int32 b = 7;\n"
        "        return a * b;\n"
        "    }\n"
        "}\n")};
    JitRunOptions opts;
    opts.sourceRoot = p.root.string();
    opts.entryMethod = "demo.Calc.main";
    EXPECT_EQ(runJit(opts), 42);
}

// A void entry runs for side effects and yields exit code 0.
TEST(JitHost, RunsVoidEntry) {
    TempProgram p{writeProgram("demo", "Noop.cajeta",
        "package demo;\n"
        "public class Noop {\n"
        "    public static void main() {\n"
        "        int32 x = 1;\n"
        "    }\n"
        "}\n")};
    JitRunOptions opts;
    opts.sourceRoot = p.root.string();
    opts.entryMethod = "demo.Noop.main";
    EXPECT_EQ(runJit(opts), 0);
}

// Entry method that doesn't exist → exit code 1 (compile succeeds, lookup fails).
TEST(JitHost, MissingEntryReturnsOne) {
    TempProgram p{writeProgram("demo", "Prog.cajeta",
        "package demo;\n"
        "public class Prog {\n"
        "    public static int32 main() { return 1; }\n"
        "}\n")};
    JitRunOptions opts;
    opts.sourceRoot = p.root.string();
    opts.entryMethod = "demo.Prog.doesNotExist";
    EXPECT_EQ(runJit(opts), 1);
}

// A source root that isn't a directory → exit code 2 (before any compilation).
TEST(JitHost, NonexistentSourceRootReturnsTwo) {
    JitRunOptions opts;
    opts.sourceRoot =
        (fs::temp_directory_path() / "cajeta_no_such_root_zzqq").string();
    opts.entryMethod = "demo.Prog.main";
    EXPECT_EQ(runJit(opts), 2);
}
