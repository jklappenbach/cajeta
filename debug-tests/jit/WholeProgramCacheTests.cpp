//
// fast-debug-launch Unit 4 — whole-program merged-module cache (plan 4.1.x).
// With JitRunOptions.cacheDir set, the first launch writes the merged
// program's bitcode + sidecars to a content-keyed slot; a relaunch with the
// same sources/flags/entry loads it and skips the Compiler entirely. Any
// doubt — edit, flag flip, corrupt slot — silently falls back to a full
// compile. Never stale, never a launch failure.
//
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

#include "../TempProgram.h"
#include "cajeta/dbg/DebugLocTable.h"
#include "cajeta/jit/CajetaJitHost.h"

namespace fs = std::filesystem;
using cajeta::debugtest::TempProgram;
using cajeta::jit::Breakpoint;
using cajeta::jit::JitRunOptions;
using cajeta::jit::JitRunResult;
using cajeta::jit::runJit;
using cajeta::jit::startDebugSession;

namespace {

const char* kMainSrc =
    "package demo;\n"
    "public class Prog {\n"
    "    public static int32 main() {\n"
    "        int32 a = Helper.six();\n"   // line 4
    "        int32 b = 7;\n"              // line 5
    "        return a * b;\n"             // line 6
    "    }\n"
    "}\n";

const char* kHelperSrc =
    "package demo;\n"
    "public class Helper {\n"
    "    public static int32 six() {\n"
    "        return 6;\n"
    "    }\n"
    "}\n";

// A fixture with two sources (the merge path matters) and its own fresh
// cache dir, both cleaned on scope exit.
struct CachedProgram {
    TempProgram prog;
    fs::path cacheDir;

    CachedProgram() : prog("demo", "Prog.cajeta", kMainSrc) {
        std::ofstream out(prog.root / "demo" / "Helper.cajeta");
        out << kHelperSrc;
        static std::mt19937_64 rng(std::random_device{}());
        cacheDir = fs::temp_directory_path()
                 / ("cajeta_jitcache_test_" + std::to_string(rng()));
        fs::create_directories(cacheDir);
    }
    ~CachedProgram() {
        std::error_code ec;
        fs::remove_all(cacheDir, ec);
    }

    JitRunOptions opts(bool debugInfo = false) const {
        JitRunOptions o;
        o.sourceRoot = prog.sourceRoot();
        o.entryMethod = "demo.Prog.main";
        o.debugInfo = debugInfo;
        o.cacheDir = cacheDir.string();
        return o;
    }
};

} // namespace

// 4.1.1 — second identical launch is a hit and still runs correctly.
TEST(WholeProgramCache, WarmRelaunchHitsAndRuns) {
    CachedProgram p;

    JitRunResult r1;
    ASSERT_EQ(runJit(p.opts(), &r1), 42);
    EXPECT_FALSE(r1.cacheHit);

    JitRunResult r2;
    ASSERT_EQ(runJit(p.opts(), &r2), 42);
    EXPECT_TRUE(r2.cacheHit);
}

// 4.1.2 — cold vs warm equivalence under -g: identical safepoint counts and
// idsForLine, and a warm DEBUG SESSION still parks at a breakpoint.
TEST(WholeProgramCache, ColdVsWarmDebugEquivalence) {
    CachedProgram p;

    // The table stores the REMAPPED declaring-file path (external-debug §6),
    // so match by basename + line exactly the way breakpoint arming does.
    auto idsForBaseLine = [](const std::string& base, int line) {
        std::vector<int32_t> ids;
        const auto& t = cajeta::dbg::globalDbgLocTable();
        for (int32_t id : t.assignedIds()) {   // sparse ranges: no 0..size()
            const auto& loc = t.at(id);
            if (loc.line != line || loc.file.empty()) continue;
            if (fs::path(loc.file).filename().string() == base)
                ids.push_back(id);
        }
        return ids;
    };

    JitRunResult cold;
    ASSERT_EQ(runJit(p.opts(true), &cold), 42);
    ASSERT_FALSE(cold.cacheHit);
    auto coldIds = idsForBaseLine("Prog.cajeta", 5);
    ASSERT_FALSE(coldIds.empty());

    JitRunResult warm;
    ASSERT_EQ(runJit(p.opts(true), &warm), 42);
    ASSERT_TRUE(warm.cacheHit);
    EXPECT_EQ(warm.entrySafepointsEmitted, cold.entrySafepointsEmitted);
    auto warmIds = idsForBaseLine("Prog.cajeta", 5);
    EXPECT_EQ(warmIds, coldIds);

    // Warm debug session: breakpoint on line 5 parks, resumes, exits 42.
    std::string err;
    auto session = startDebugSession(p.opts(), {Breakpoint{"Prog.cajeta", 5}},
                                     &err);
    ASSERT_NE(session, nullptr) << err;
    auto stop = session->controller().waitForStop();
    EXPECT_EQ(cajeta::dbg::globalDbgLocTable().at(stop.locId).line, 5);
    session->controller().resume();
    EXPECT_EQ(session->join(), 42);
}

// 4.1.3 — an edit changes the key: full compile, then the NEW content is a
// hit on the following launch.
TEST(WholeProgramCache, EditInvalidatesThenRepopulates) {
    CachedProgram p;

    JitRunResult r1;
    ASSERT_EQ(runJit(p.opts(), &r1), 42);

    {
        std::ofstream out(p.prog.root / "demo" / "Helper.cajeta",
                          std::ios::app);
        out << "// touched\n";
    }

    JitRunResult r2;
    ASSERT_EQ(runJit(p.opts(), &r2), 42);
    EXPECT_FALSE(r2.cacheHit);

    JitRunResult r3;
    ASSERT_EQ(runJit(p.opts(), &r3), 42);
    EXPECT_TRUE(r3.cacheHit);
}

// 4.1.4 — a cache-keyed flag flip (debugInfo) misses; never serves the other
// flavor's module.
TEST(WholeProgramCache, FlagFlipInvalidates) {
    CachedProgram p;

    JitRunResult plain;
    ASSERT_EQ(runJit(p.opts(false), &plain), 42);

    JitRunResult dbg;
    ASSERT_EQ(runJit(p.opts(true), &dbg), 42);
    EXPECT_FALSE(dbg.cacheHit);
    EXPECT_GT(dbg.entrySafepointsEmitted, 0);  // really the -g flavor
}

// 4.1.5 — a corrupt slot silently falls back to a full compile.
TEST(WholeProgramCache, CorruptSlotFallsBackToCompile) {
    CachedProgram p;

    JitRunResult r1;
    ASSERT_EQ(runJit(p.opts(), &r1), 42);

    // Garbage every pooled module bitcode under the cache tree.
    int corrupted = 0;
    for (auto& entry : fs::recursive_directory_iterator(p.cacheDir)) {
        if (entry.is_regular_file() &&
            entry.path().extension() == ".bc") {
            std::ofstream out(entry.path(),
                              std::ios::binary | std::ios::trunc);
            out << "not bitcode";
            corrupted++;
        }
    }
    ASSERT_GT(corrupted, 0) << "no slot written under " << p.cacheDir;

    JitRunResult r2;
    ASSERT_EQ(runJit(p.opts(), &r2), 42);
    EXPECT_FALSE(r2.cacheHit);
    // (Pooled OBJECTS may still legitimately serve here: they are
    // content-addressed by module IR digest, so a serve proves identity.)
}

// 4.1.6 — String[] entry on a HIT launch gets usable args without the type
// world (the ABI rides the slot's sidecar, not CajetaType::of).
TEST(WholeProgramCache, StringArrayEntryUsableOnHit) {
    TempProgram prog("demo", "ArgsProg.cajeta",
        "package demo;\n"
        "public class ArgsProg {\n"
        "    public static int32 main(String[] args) {\n"
        "        return (int32) args.count();\n"
        "    }\n"
        "}\n");
    static std::mt19937_64 rng(std::random_device{}());
    fs::path cacheDir = fs::temp_directory_path()
                      / ("cajeta_jitcache_test_" + std::to_string(rng()));
    fs::create_directories(cacheDir);

    JitRunOptions opts;
    opts.sourceRoot = prog.sourceRoot();
    opts.entryMethod = "demo.ArgsProg.main";
    opts.cacheDir = cacheDir.string();

    opts.programArgs = {"alpha", "beta", "gamma"};
    JitRunResult r1;
    ASSERT_EQ(runJit(opts, &r1), 3);
    ASSERT_FALSE(r1.cacheHit);

    opts.programArgs = {"x", "y"};
    JitRunResult r2;
    ASSERT_EQ(runJit(opts, &r2), 2);
    EXPECT_TRUE(r2.cacheHit);

    std::error_code ec;
    fs::remove_all(cacheDir, ec);
}

// ---- fast-debug-launch Unit 6: ORC ObjectCache (plan 6.1.x) ----------------
// The warm residue after Unit 4 is LLJIT materialization (instruction
// selection over the whole merged module). program.o beside the slot converts
// that into loading one object file. The slot key already content-addresses
// the IR, so a stale object is structurally impossible — pinned behaviorally
// below by asserting the EDITED program's result, not just a flag.

TEST(ObjectCache, SecondLaunchServesObjectFromCache) {
    CachedProgram p;

    JitRunResult r1;
    ASSERT_EQ(runJit(p.opts(), &r1), 42);
    EXPECT_FALSE(r1.objectCacheHit);

    JitRunResult r2;
    ASSERT_EQ(runJit(p.opts(), &r2), 42);
    EXPECT_TRUE(r2.cacheHit);
    EXPECT_TRUE(r2.objectCacheHit);
}

TEST(ObjectCache, IrChangeNeverServesStaleObject) {
    CachedProgram p;

    JitRunResult r1;
    ASSERT_EQ(runJit(p.opts(), &r1), 42);

    // Change behavior: six() -> five(). A stale object would still return 42.
    {
        std::ofstream out(p.prog.root / "demo" / "Helper.cajeta",
                          std::ios::trunc);
        out << "package demo;\n"
               "public class Helper {\n"
               "    public static int32 six() {\n"
               "        return 5;\n"
               "    }\n"
               "}\n";
    }

    JitRunResult r2;
    ASSERT_EQ(runJit(p.opts(), &r2), 35);   // 5 * 7 — behavior, not flags
    EXPECT_FALSE(r2.cacheHit);
    EXPECT_FALSE(r2.objectCacheHit);

    JitRunResult r3;
    ASSERT_EQ(runJit(p.opts(), &r3), 35);
    EXPECT_TRUE(r3.cacheHit);
    EXPECT_TRUE(r3.objectCacheHit);
}


// ---- resident-debug-server Unit 2: per-module delivery (plan 2.1.3) -------
// Modules reach the LLJIT individually, content-addressed by IR digest in a
// shared pool. An edit therefore recompiles the edited module's object and
// SERVES the rest — the property that makes edit relaunches cheap.

TEST(PerModuleDelivery, EditRecompilesOnlyTheEditedModulesObject) {
    CachedProgram p;

    JitRunResult r1;
    ASSERT_EQ(runJit(p.opts(), &r1), 42);   // populates the pools
    EXPECT_GT(r1.moduleObjectsCompiled, 0);
    EXPECT_EQ(r1.moduleObjectsServed, 0);

    {
        std::ofstream out(p.prog.root / "demo" / "Helper.cajeta",
                          std::ios::trunc);
        out << "package demo;\n"
               "public class Helper {\n"
               "    public static int32 six() {\n"
               "        return 5;\n"
               "    }\n"
               "}\n";
    }

    JitRunResult r2;
    ASSERT_EQ(runJit(p.opts(), &r2), 35);
    EXPECT_FALSE(r2.cacheHit);
    // Unchanged modules' IR digests match the pool: served, not recompiled.
    // Across FRESH processes exactly one module recompiles (verified by
    // hand on the pool); inside ONE test process the reused stdlib module
    // drifts between compiles (it accumulates run-1 instantiations), so it
    // may re-digest too — hence <= 2, and the same total module count.
    EXPECT_GT(r2.moduleObjectsServed, 0);
    EXPECT_LE(r2.moduleObjectsCompiled, 2);
    EXPECT_EQ(r2.moduleObjectsServed + r2.moduleObjectsCompiled,
              r1.moduleObjectsCompiled);
}
