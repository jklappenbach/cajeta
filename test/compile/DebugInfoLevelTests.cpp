// external-debug Unit 1 — the --debug-info level reaches the compiler.
//
// Before this unit, `flags.debugInfo` could only be set by the JIT host: the
// frontend rejected --debug-info outright, so an AOT binary never carried a
// safepoint or a local record, and nothing external could debug it (spec §1.2).
//
// Pins:
//   1.1.1  --debug-info=off|line|full parses; an unknown value errors and names
//          the accepted set.
//   1.1.2  `full` sets debugInfo; `line` and `off` leave it false. `off` also
//          drops the line-info shadow stack; `line` keeps it.
//   1.1.3  A `full` compile emits __cajeta_dbg_safepoint + __cajeta_dbg_local
//          into the IR; `line` and `off` emit neither.
//   1.1.6  The level keys the IR cache, so flipping it recompiles rather than
//          re-publishing a cached artifact.

#include <gtest/gtest.h>

#include "cajeta/compile/CacheManifest.h"
#include "cajeta/compile/CompilerMode.h"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>

#ifdef _WIN32
#  define CAJETA_DEVNULL "NUL"
#else
#  define CAJETA_DEVNULL "/dev/null"
#endif

using cajeta::CompilerFlags;
using cajeta::DebugInfo;

namespace {

    namespace fs = std::filesystem;

    // The in-tree compiler binary (mirrors ReproducibleIrTests' helper).
    std::string compilerPath() {
        const char* envRoot = std::getenv("CAJETA_SOURCE_ROOT");
        std::string r = (envRoot && *envRoot) ? envRoot :
#ifdef CAJETA_SOURCE_ROOT_DEFAULT
            CAJETA_SOURCE_ROOT_DEFAULT;
#else
            ".";
#endif
        return r + "/build/src/cajeta";
    }

    struct TmpProject {
        fs::path base;
        fs::path sourceRoot;
        fs::path buildRoot;
    };

    TmpProject makeTmpProject(const std::string& tag) {
        static std::mt19937_64 rng(std::random_device{}());
        auto base = fs::temp_directory_path()
                  / ("cajeta_dbginfo_" + tag + "_" + std::to_string(rng()));
        auto src = base / "src" / "demo";
        auto build = base / "build";
        fs::create_directories(src);
        fs::create_directories(build);
        std::ofstream out(src / "Hello.cajeta");
        // A local plus a statement after it: `full` must produce both a
        // safepoint (statement boundary) and a local record.
        out << "package demo;\n"
            << "public final class Hello {\n"
            << "    public static int32 run() {\n"
            << "        int32 n = 7;\n"
            << "        n = n + 1;\n"
            << "        return n;\n"
            << "    }\n"
            << "}\n";
        return TmpProject{base, base / "src", build};
    }

    // Emit IR at the given level; return every .ll the compiler wrote,
    // concatenated. Empty string means the compile failed.
    std::string emitIrAtLevel(const TmpProject& proj, const std::string& level) {
        std::string cmd = compilerPath()
            + " --debug-info=" + level
            + " --emit=ir demo.Hello.run "
            + proj.sourceRoot.string() + " "
            + proj.buildRoot.string()
            + " > " CAJETA_DEVNULL " 2>&1";
        if (std::system(cmd.c_str()) != 0) return "";

        std::string all;
        for (const auto& e : fs::recursive_directory_iterator(proj.buildRoot)) {
            if (!e.is_regular_file() || e.path().extension() != ".ll") continue;
            std::ifstream f(e.path());
            all.append(std::istreambuf_iterator<char>(f),
                       std::istreambuf_iterator<char>());
        }
        return all;
    }

    // Assert on CALL SITES, never on the bare symbol: the linked-in runtime
    // module *defines* __cajeta_dbg_safepoint / _local / __cajeta_line_enter,
    // so their names appear in the IR at every level. Only codegen emitting a
    // call to them says the level took effect.
    bool calls(const std::string& ir, const std::string& fn) {
        return ir.find("call void @" + fn + "(") != std::string::npos;
    }

    std::string flagValue(const CompilerFlags& f, const std::string& key) {
        for (const auto& p : cajeta::cacheFlagPairs(f, "exe", ""))
            if (p.first == key) return p.second;
        return "<absent>";
    }

} // namespace

// ---- 1.1.1 / 1.1.2 — parse + derived flags --------------------------

TEST(DebugInfoLevelTests, parsesTheThreeLevels) {
    CompilerFlags f;
    std::string err;

    ASSERT_TRUE(cajeta::applyDebugInfo("off", f, &err)) << err;
    EXPECT_EQ(f.debugInfoLevel, DebugInfo::Off);

    ASSERT_TRUE(cajeta::applyDebugInfo("line", f, &err)) << err;
    EXPECT_EQ(f.debugInfoLevel, DebugInfo::Line);

    ASSERT_TRUE(cajeta::applyDebugInfo("full", f, &err)) << err;
    EXPECT_EQ(f.debugInfoLevel, DebugInfo::Full);
}

TEST(DebugInfoLevelTests, unknownValueErrorsAndNamesTheAcceptedSet) {
    CompilerFlags f;
    std::string err;
    EXPECT_FALSE(cajeta::applyDebugInfo("yes", f, &err));
    EXPECT_NE(err.find("off"), std::string::npos) << err;
    EXPECT_NE(err.find("line"), std::string::npos) << err;
    EXPECT_NE(err.find("full"), std::string::npos) << err;
    // A rejected value leaves the flags untouched.
    EXPECT_EQ(f.debugInfoLevel, CompilerFlags{}.debugInfoLevel);
}

TEST(DebugInfoLevelTests, onlyFullSetsTheDebugInfoBool) {
    CompilerFlags f;
    ASSERT_TRUE(cajeta::applyDebugInfo("full", f, nullptr));
    EXPECT_TRUE(f.debugInfo);
    EXPECT_TRUE(f.lineInfo);

    ASSERT_TRUE(cajeta::applyDebugInfo("line", f, nullptr));
    EXPECT_FALSE(f.debugInfo);
    EXPECT_TRUE(f.lineInfo) << "line keeps the shadow stack";

    ASSERT_TRUE(cajeta::applyDebugInfo("off", f, nullptr));
    EXPECT_FALSE(f.debugInfo);
    EXPECT_FALSE(f.lineInfo) << "off drops the shadow stack too";
}

// The compiler default matches today's behavior: no safepoints, shadow stack on.
TEST(DebugInfoLevelTests, defaultLevelIsLine) {
    CompilerFlags f;
    EXPECT_EQ(f.debugInfoLevel, DebugInfo::Line);
    EXPECT_FALSE(f.debugInfo);
    EXPECT_TRUE(f.lineInfo);
}

// ---- 1.1.3 — the level reaches codegen through the CLI ---------------

TEST(DebugInfoLevelTests, fullEmitsSafepointsAndLocalRecords) {
    auto proj = makeTmpProject("full");
    auto ir = emitIrAtLevel(proj, "full");
    ASSERT_FALSE(ir.empty()) << "--debug-info=full compile failed";
    EXPECT_TRUE(calls(ir, "__cajeta_dbg_safepoint"));
    EXPECT_TRUE(calls(ir, "__cajeta_dbg_local"));
    fs::remove_all(proj.base);
}

TEST(DebugInfoLevelTests, lineEmitsNoDebugRecords) {
    auto proj = makeTmpProject("line");
    auto ir = emitIrAtLevel(proj, "line");
    ASSERT_FALSE(ir.empty()) << "--debug-info=line compile failed";
    EXPECT_FALSE(calls(ir, "__cajeta_dbg_safepoint"));
    EXPECT_FALSE(calls(ir, "__cajeta_dbg_local"));
    // ...but it keeps the shadow stack, which is what `line` buys.
    EXPECT_TRUE(calls(ir, "__cajeta_line_enter"));
    fs::remove_all(proj.base);
}

TEST(DebugInfoLevelTests, offEmitsNeitherRecordsNorShadowStack) {
    auto proj = makeTmpProject("off");
    auto ir = emitIrAtLevel(proj, "off");
    ASSERT_FALSE(ir.empty()) << "--debug-info=off compile failed";
    EXPECT_FALSE(calls(ir, "__cajeta_dbg_safepoint"));
    EXPECT_FALSE(calls(ir, "__cajeta_dbg_local"));
    EXPECT_FALSE(calls(ir, "__cajeta_line_enter"));
    fs::remove_all(proj.base);
}

TEST(DebugInfoLevelTests, cliRejectsAnUnknownLevel) {
    auto proj = makeTmpProject("bad");
    std::string cmd = compilerPath()
        + " --debug-info=yes --emit=ir demo.Hello.run "
        + proj.sourceRoot.string() + " " + proj.buildRoot.string()
        + " > " CAJETA_DEVNULL " 2>&1";
    EXPECT_NE(std::system(cmd.c_str()), 0);
    fs::remove_all(proj.base);
}

// ---- 1.1.6 — the level keys the cache --------------------------------

TEST(DebugInfoLevelTests, cacheFlagPairsCarryTheLevelNotABool) {
    CompilerFlags off, line, full;
    ASSERT_TRUE(cajeta::applyDebugInfo("off",  off,  nullptr));
    ASSERT_TRUE(cajeta::applyDebugInfo("line", line, nullptr));
    ASSERT_TRUE(cajeta::applyDebugInfo("full", full, nullptr));

    EXPECT_EQ(flagValue(off,  "debug-info"), "off");
    EXPECT_EQ(flagValue(line, "debug-info"), "line");
    EXPECT_EQ(flagValue(full, "debug-info"), "full");

    // A bool would collapse off and line into the same cache key, and the
    // cached artifact of one would be re-published for the other.
    EXPECT_NE(flagValue(off, "debug-info"), flagValue(line, "debug-info"));
}
