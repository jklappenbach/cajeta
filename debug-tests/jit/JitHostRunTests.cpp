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

// --- canonical entry signature (run-config-ergonomics Unit 7 / spec §7) -----
// The application entry point is `static int32 main(String[] args)` — the same
// shape Java and C++ use. findEntryMangled bound only `Class.method()`, so every
// conventional entry failed with "could not find static no-arg entry" and no
// real project could be debugged. The binary path already accepts both shapes
// (Compiler::emitCMainShim) and marshals argv via __cajeta_args_make; these pin
// the JIT path to the same rule.

// 7.1.1 / spec 7.3.1 — the canonical signature launches at all.
TEST(JitHost, RunsCanonicalStringArrayEntry) {
    TempProgram p{writeProgram("demo", "ArgsProg.cajeta",
        "package demo;\n"
        "public class ArgsProg {\n"
        "    public static int32 main(String[] args) {\n"
        "        return 42;\n"
        "    }\n"
        "}\n")};
    JitRunOptions opts;
    opts.sourceRoot = p.root.string();
    opts.entryMethod = "demo.ArgsProg.main";
    EXPECT_EQ(runJit(opts), 42);
}

// 7.1.2 / spec 7.3.2 — the no-arg form is untouched. (Also covered by every
// other test in this file; stated explicitly so the guard is named.)
TEST(JitHost, StillRunsNoArgEntryAfterWideningTheMatch) {
    TempProgram p{writeProgram("demo", "NoArgProg.cajeta",
        "package demo;\n"
        "public class NoArgProg {\n"
        "    public static int32 main() {\n"
        "        return 7;\n"
        "    }\n"
        "}\n")};
    JitRunOptions opts;
    opts.sourceRoot = p.root.string();
    opts.entryMethod = "demo.NoArgProg.main";
    EXPECT_EQ(runJit(opts), 7);
}

// 7.1.3 / spec 7.3.3 — `args` is a USABLE String[] inside the debuggee, not
// null or garbage. With no program args the count is 0.
TEST(JitHost, StringArrayEntryReceivesAUsableEmptyArray) {
    TempProgram p{writeProgram("demo", "CountProg.cajeta",
        "package demo;\n"
        "public class CountProg {\n"
        "    public static int32 main(String[] args) {\n"
        "        return (int32) args.count();\n"
        "    }\n"
        "}\n")};
    JitRunOptions opts;
    opts.sourceRoot = p.root.string();
    opts.entryMethod = "demo.CountProg.main";
    EXPECT_EQ(runJit(opts), 0);
}

// 7.1.3b — programArgs (a reserved field until now) actually reach the entry.
TEST(JitHost, StringArrayEntryReceivesForwardedProgramArgs) {
    TempProgram p{writeProgram("demo", "CountArgs.cajeta",
        "package demo;\n"
        "public class CountArgs {\n"
        "    public static int32 main(String[] args) {\n"
        "        return (int32) args.count();\n"
        "    }\n"
        "}\n")};
    JitRunOptions opts;
    opts.sourceRoot = p.root.string();
    opts.entryMethod = "demo.CountArgs.main";
    opts.programArgs = {"alpha", "beta", "gamma"};
    EXPECT_EQ(runJit(opts), 3);
}

// 7.1.5 / spec 7.2.5 — a parameterized entry must NOT be invoked through a
// no-arg function pointer (the UB the original narrowing existed to prevent).
// A correctly-typed call returns the entry's real value; calling through the
// wrong pointer type would not reliably produce 99.
TEST(JitHost, ParameterizedEntryReturnsItsRealValueNotGarbage) {
    TempProgram p{writeProgram("demo", "RetProg.cajeta",
        "package demo;\n"
        "public class RetProg {\n"
        "    public static int32 main(String[] args) {\n"
        "        int32 a = 90;\n"
        "        int32 b = 9;\n"
        "        return a + b;\n"
        "    }\n"
        "}\n")};
    JitRunOptions opts;
    opts.sourceRoot = p.root.string();
    opts.entryMethod = "demo.RetProg.main";
    EXPECT_EQ(runJit(opts), 99);
}
