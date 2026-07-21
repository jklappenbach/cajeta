//
// resident-debug-server Unit 3 — per-module loc-id ranges (plan 3.1.x).
// Dense global loc ids made one edit shift every later module's safepoint
// constants, churning their IR and defeating the object pool under -g.
// Each module now owns an id RANGE (registry-stable across rebuilds), so an
// edit leaves other modules' -g IR byte-identical — pinned here via pool
// serve counters — while breakpoints still resolve through the sparse table.
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

const char* kA =
    "package demo;\n"
    "public class A {\n"
    "    public static int32 main() {\n"
    "        int32 x = B.f();\n"      // line 4
    "        int32 y = C.g();\n"      // line 5
    "        return x + y;\n"         // line 6
    "    }\n"
    "}\n";

const char* kB =
    "package demo;\n"
    "public class B {\n"
    "    public static int32 f() {\n"
    "        return 40;\n"
    "    }\n"
    "}\n";

const char* kC =
    "package demo;\n"
    "public class C {\n"
    "    public static int32 g() {\n"
    "        return 2;\n"             // line 4
    "    }\n"
    "}\n";

struct RangedProgram {
    TempProgram prog;
    fs::path cacheDir;

    RangedProgram() : prog("demo", "A.cajeta", kA) {
        { std::ofstream out(prog.root / "demo" / "B.cajeta"); out << kB; }
        { std::ofstream out(prog.root / "demo" / "C.cajeta"); out << kC; }
        static std::mt19937_64 rng(std::random_device{}());
        cacheDir = fs::temp_directory_path()
                 / ("cajeta_locrange_test_" + std::to_string(rng()));
        fs::create_directories(cacheDir);
    }
    ~RangedProgram() {
        std::error_code ec;
        fs::remove_all(cacheDir, ec);
    }

    JitRunOptions opts() const {
        JitRunOptions o;
        o.sourceRoot = prog.sourceRoot();
        o.entryMethod = "demo.A.main";
        o.debugInfo = true;   // ranges matter exactly when safepoints exist
        o.cacheDir = cacheDir.string();
        return o;
    }
};

} // namespace

// 3.1.1 — editing B (changing its STATEMENT COUNT) leaves other modules'
// -g IR byte-identical: their pooled objects serve on the rebuild.
TEST(LocRanges, EditKeepsOtherModulesObjectsServable) {
    RangedProgram p;

    JitRunResult r1;
    ASSERT_EQ(runJit(p.opts(), &r1), 42);
    ASSERT_FALSE(r1.cacheHit);
    ASSERT_GT(r1.moduleObjectsCompiled, 0);

    {
        // More statements in B: under dense global ids this shifted every
        // later module's safepoint constants.
        std::ofstream out(p.prog.root / "demo" / "B.cajeta", std::ios::trunc);
        out << "package demo;\n"
               "public class B {\n"
               "    public static int32 f() {\n"
               "        int32 t = 10;\n"
               "        t = t * 4;\n"
               "        return t;\n"
               "    }\n"
               "}\n";
    }

    JitRunResult r2;
    ASSERT_EQ(runJit(p.opts(), &r2), 42);
    EXPECT_FALSE(r2.cacheHit);
    // At least one non-edited module's object must serve (C and/or A; the
    // in-process stdlib module may drift — see PerModuleDelivery notes).
    EXPECT_GE(r2.moduleObjectsServed, 1)
        << "loc-id churn defeated the object pool for unchanged modules";
}

// 3.1.2 — breakpoints and stackTrace resolve with RANGED (sparse) ids: a
// session over ranged modules parks on the right line and reports it.
TEST(LocRanges, BreakpointsResolveWithRangedIds) {
    RangedProgram p;

    std::string err;
    auto session = startDebugSession(p.opts(), {Breakpoint{"C.cajeta", 4}},
                                     &err);
    ASSERT_NE(session, nullptr) << err;
    auto stop = session->controller().waitForStop();
    const auto& loc = cajeta::dbg::globalDbgLocTable().at(stop.locId);
    EXPECT_EQ(loc.line, 4);
    EXPECT_EQ(fs::path(loc.file).filename().string(), "C.cajeta");
    session->controller().resume();
    EXPECT_EQ(session->join(), 42);
}
