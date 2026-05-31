//
// End-to-end CP2 tests: with --debug-info on, statement-boundary codegen emits
// __cajeta_dbg_safepoint(loc_id) calls (one per statement), and they execute at
// runtime. With it off, none are emitted and none run. Drives the full
// parse → codegen → merge → LLJIT → invoke pipeline via runJit, checking both
// the static (IR) count inside the entry and the runtime (executed) count.
//
#include <gtest/gtest.h>

#include "cajeta/jit/CajetaJitHost.h"
#include "../TempProgram.h"

using cajeta::jit::JitRunOptions;
using cajeta::jit::JitRunResult;
using cajeta::jit::runJit;
using cajeta::debugtest::TempProgram;

namespace {

// A 3-statement entry: two locals + a return. With debug info on this yields
// exactly 3 safepoints inside the entry, all executed once.
const char* kThreeStmtProgram =
    "package demo;\n"
    "public class Calc {\n"
    "    public static int32 main() {\n"
    "        int32 a = 6;\n"
    "        int32 b = 7;\n"
    "        return a * b;\n"
    "    }\n"
    "}\n";

} // namespace

TEST(SafepointCodegen, EmitsOneSafepointPerStatement) {
    TempProgram p("demo", "Calc.cajeta", kThreeStmtProgram);
    JitRunOptions opts;
    opts.sourceRoot = p.sourceRoot();
    opts.entryMethod = "demo.Calc.main";
    opts.debugInfo = true;

    JitRunResult result;
    int rc = runJit(opts, &result);

    EXPECT_EQ(rc, 42);  // program still computes correctly
    EXPECT_EQ(result.entrySafepointsEmitted, 3);   // one per statement (static)
    EXPECT_EQ(result.safepointsExecuted, 3);       // each ran once (runtime)
}

TEST(SafepointCodegen, NoSafepointsWhenDebugInfoOff) {
    TempProgram p("demo", "Calc.cajeta", kThreeStmtProgram);
    JitRunOptions opts;
    opts.sourceRoot = p.sourceRoot();
    opts.entryMethod = "demo.Calc.main";
    opts.debugInfo = false;

    JitRunResult result;
    int rc = runJit(opts, &result);

    EXPECT_EQ(rc, 42);
    EXPECT_EQ(result.entrySafepointsEmitted, 0);
    EXPECT_EQ(result.safepointsExecuted, 0);
}
