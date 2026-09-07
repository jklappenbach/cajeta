//
// ownership-title-classifier 1.1.3 — the corpus instrument, tested as a
// program: build `tools/ir-corpus-diff` with the compiler under test, then
// run it on fixture `.ll` trees.
//
//   identical inputs (modulo SSA numbering)      → every function `same`, exit 0
//   a real instruction change                    → `changed`, the - / + lines, exit 1
//   a constant branch the baseline computed at
//   run time and the candidate folded            → `folded`, not `changed` (needs `opt`)
//   `count`                                      → the instruction total
//
// The fixtures live under the repo's tmp/ (never /tmp), one directory per
// test process.
//

#include "gtest/gtest.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

std::string sourceRoot() {
    const char* envRoot = std::getenv("CAJETA_SOURCE_ROOT");
    if (envRoot && *envRoot) return envRoot;
#ifdef CAJETA_SOURCE_ROOT_DEFAULT
    return CAJETA_SOURCE_ROOT_DEFAULT;
#else
    return ".";
#endif
}

int exitCodeOf(int status) {
#ifdef _WIN32
    return status;
#else
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

void writeFile(const std::string& path, const std::string& text) {
    std::ofstream out(path);
    out << text;
}

std::string readFile(const std::string& path) {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

struct Tool {
    std::string root;      // repo
    std::string work;      // tmp/ir-corpus-diff-test/<pid>
    std::string exe;       // the built tool
    std::string opt;       // opt binary, or empty

    bool build() {
        root = sourceRoot();
        work = root + "/tmp/ir-corpus-diff-test/" + std::to_string((long) getpid());
        std::string cmd = "mkdir -p " + work + "/out && " + root + "/build/src/cajeta --emit=exe -o "
            + work + "/ir-corpus-diff ircorpusdiff.Main.run " + root + "/tools/ir-corpus-diff/src "
            + work + "/out > " + work + "/build.log 2>&1";
        int st = std::system(("mkdir -p " + work + " && " + cmd).c_str());
        exe = work + "/ir-corpus-diff";
        const char* llvmBin = std::getenv("CAJETA_LLVM_BIN");
        if (llvmBin && *llvmBin) {
            std::string o = std::string(llvmBin) + "/opt";
            struct stat sb;
            if (::stat(o.c_str(), &sb) == 0) opt = o;
        }
        return exitCodeOf(st) == 0;
    }

    // Run the tool; returns the exit code, fills `out` with stdout.
    int run(const std::string& args, std::string& out) {
        std::string log = work + "/run.log";
        int st = std::system((exe + " " + args + " > " + log + " 2>&1").c_str());
        out = readFile(log);
        return exitCodeOf(st);
    }
};

// A module with two functions. `f` reads a runtime value; `g` is a plain add.
const char* BASE_LL =
    "; ModuleID = 'm'\n"
    "define i32 @f(i32 %0, i1 %1) {\n"
    "entry:\n"
    "  %2 = icmp ne i64 0, 0\n"
    "  br i1 %2, label %a, label %b\n"
    "a:                                     ; preds = %entry\n"
    "  %3 = add i32 %0, 1\n"
    "  br label %b\n"
    "b:                                     ; preds = %a, %entry\n"
    "  %4 = phi i32 [ %3, %a ], [ %0, %entry ]\n"
    "  ret i32 %4\n"
    "}\n"
    "define i32 @g(i32 %0) {\n"
    "entry:\n"
    "  %1 = add i32 %0, 7\n"
    "  ret i32 %1\n"
    "}\n";

// The same program, renumbered: what a second emission of identical code
// looks like when an unrelated instruction moved the SSA counter.
const char* SAME_RENUMBERED_LL =
    "; ModuleID = 'm'\n"
    "define i32 @f(i32 %0, i1 %1) {\n"
    "entry:\n"
    "  %12 = icmp ne i64 0, 0\n"
    "  br i1 %12, label %a, label %b\n"
    "a:\n"
    "  %13 = add i32 %0, 1\n"
    "  br label %b\n"
    "b:\n"
    "  %14 = phi i32 [ %13, %a ], [ %0, %entry ]\n"
    "  ret i32 %14\n"
    "}\n"
    "define i32 @g(i32 %0) {\n"
    "entry:\n"
    "  %9 = add i32 %0, 7\n"
    "  ret i32 %9\n"
    "}\n";

// `g` gained an instruction.
const char* CHANGED_LL =
    "define i32 @f(i32 %0, i1 %1) {\n"
    "entry:\n"
    "  %2 = icmp ne i64 0, 0\n"
    "  br i1 %2, label %a, label %b\n"
    "a:\n"
    "  %3 = add i32 %0, 1\n"
    "  br label %b\n"
    "b:\n"
    "  %4 = phi i32 [ %3, %a ], [ %0, %entry ]\n"
    "  ret i32 %4\n"
    "}\n"
    "define i32 @g(i32 %0) {\n"
    "entry:\n"
    "  %1 = add i32 %0, 7\n"
    "  %2 = mul i32 %1, 2\n"
    "  ret i32 %2\n"
    "}\n";

// `f` with the branch condition already a constant — the shape a classifier
// that folds a static title flag emits. Different text, same program.
const char* FOLDED_LL =
    "define i32 @f(i32 %0, i1 %1) {\n"
    "entry:\n"
    "  br i1 false, label %a, label %b\n"
    "a:\n"
    "  %2 = add i32 %0, 1\n"
    "  br label %b\n"
    "b:\n"
    "  %3 = phi i32 [ %2, %a ], [ %0, %entry ]\n"
    "  ret i32 %3\n"
    "}\n"
    "define i32 @g(i32 %0) {\n"
    "entry:\n"
    "  %1 = add i32 %0, 7\n"
    "  ret i32 %1\n"
    "}\n";

} // namespace

TEST(IrCorpusDiffToolTests, identicalModuloNumberingIsSameAndExitZero) {
    Tool t;
    ASSERT_TRUE(t.build()) << readFile(t.work + "/build.log");
    std::string base = t.work + "/same/base", cand = t.work + "/same/cand";
    std::system(("mkdir -p " + base + "/pkg " + cand + "/pkg").c_str());
    writeFile(base + "/pkg/m.ll", BASE_LL);
    writeFile(cand + "/pkg/m.ll", SAME_RENUMBERED_LL);
    std::string out;
    int code = t.run("diff " + base + " " + cand, out);
    EXPECT_EQ(code, 0) << out;
    EXPECT_NE(out.find("summary modules=1 functions=2 same=2 folded=0 changed=0 missing=0 added=0 ops=8->8"),
              std::string::npos) << out;
}

TEST(IrCorpusDiffToolTests, aRealChangeIsReportedWithItsLinesAndExitOne) {
    Tool t;
    ASSERT_TRUE(t.build()) << readFile(t.work + "/build.log");
    std::string base = t.work + "/chg/base", cand = t.work + "/chg/cand";
    std::system(("mkdir -p " + base + " " + cand).c_str());
    writeFile(base + "/m.ll", BASE_LL);
    writeFile(cand + "/m.ll", CHANGED_LL);
    std::string out;
    int code = t.run("diff " + base + " " + cand, out);
    EXPECT_EQ(code, 1) << out;
    EXPECT_NE(out.find("changed m.ll g ops 2 -> 3"), std::string::npos) << out;
    // Locals and labels share one alpha-rename space per function, numbered
    // by first occurrence: the parameter `%0` → `%v0`, the `entry` label →
    // `v1`, the add → `%v2`, the mul → `%v3`. Dataflow stays visible, names
    // do not (measured, not assumed).
    EXPECT_NE(out.find("  - ret i32 %v2"), std::string::npos) << out;
    EXPECT_NE(out.find("  + %v3 = mul i32 %v2, 2"), std::string::npos) << out;
    EXPECT_NE(out.find("same=1 folded=0 changed=1"), std::string::npos) << out;
    EXPECT_NE(out.find("ops=8->9"), std::string::npos) << out;
}

TEST(IrCorpusDiffToolTests, aFoldedConstantBranchIsFoldedNotChanged) {
    Tool t;
    ASSERT_TRUE(t.build()) << readFile(t.work + "/build.log");
    if (t.opt.empty()) GTEST_SKIP() << "no opt binary under CAJETA_LLVM_BIN";
    std::string base = t.work + "/fold/base", cand = t.work + "/fold/cand";
    std::system(("mkdir -p " + base + " " + cand).c_str());
    writeFile(base + "/m.ll", BASE_LL);
    writeFile(cand + "/m.ll", FOLDED_LL);
    std::string out;
    int code = t.run("diff " + base + " " + cand + " --opt " + t.opt, out);
    EXPECT_EQ(code, 0) << out;
    EXPECT_NE(out.find("folded m.ll f ops 6 -> 5"), std::string::npos) << out;
    EXPECT_NE(out.find("same=1 folded=1 changed=0"), std::string::npos) << out;
    // Without opt the same pair is a change: the fold is the instrument's
    // judgement, not a looser comparison.
    int code2 = t.run("diff " + base + " " + cand, out);
    EXPECT_EQ(code2, 1) << out;
    EXPECT_NE(out.find("changed m.ll f"), std::string::npos) << out;
}

TEST(IrCorpusDiffToolTests, countReportsInstructionsPerFunctionAndTotal) {
    Tool t;
    ASSERT_TRUE(t.build()) << readFile(t.work + "/build.log");
    std::string dir = t.work + "/count";
    std::system(("mkdir -p " + dir).c_str());
    writeFile(dir + "/m.ll", BASE_LL);
    std::string out;
    int code = t.run("count " + dir, out);
    EXPECT_EQ(code, 0) << out;
    EXPECT_NE(out.find("ops m.ll f 6"), std::string::npos) << out;
    EXPECT_NE(out.find("ops m.ll g 2"), std::string::npos) << out;
    EXPECT_NE(out.find("total 8"), std::string::npos) << out;
}
