// Where per-statement line marks are actually needed (profiler perf work,
// 2026-08-22).
//
// __cajeta_line_mark fires at every statement boundary and was on in every
// build, including the release flavor. Measured at -O3:
//
//                      off      enter/leave     + marks
//   realistic body    0.11 s      0.11 s        0.39 s   (3.5x)
//   tiny callee       0.05 s      0.15 s        0.47 s   (9.4x)
//
// Per-CALL enter/leave is at parity with an uninstrumented build; the marks
// are the whole cost. And it is not the probe's WORK — emptying the bodies
// changed nothing, because an opaque call at every statement boundary forbids
// inlining and folding. So marks moved to --debug-info=full.
//
// That trade needs pinning from both sides. Nothing in the suite read a
// runtime frame's `line` before this file, so the feature could have been
// silently lost — or silently kept — with every test still green.
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include <cstdint>
#include <string>
using cajeta_test::CajetaJit;

namespace {

// Returns frame 0's line number, or a negative marker if the trace is unusable.
const char* kProbe =
    "package test;\n"
    "import cajeta.error.Exception;\n"
    "import cajeta.error.StackFrame;\n"
    "public final class D {\n"
    "    public static void boom() { throw heap Exception(\"x\"); }\n"
    "    public static int32 run() {\n"
    "        try {\n"
    "            D.boom();\n"
    "            return -1;\n"
    "        } catch (Exception e) {\n"
    "            StackFrame[] fs = e.getStackTrace();\n"
    "            if (fs.count() == 0) { return -2; }\n"
    "            return fs[0].line;\n"
    "        }\n"
    "    }\n"
    "}\n";

int32_t frameLineUnder(bool fullDebugInfo) {
    CajetaJit::Options o;
    o.debugInfoEnabled = fullDebugInfo;
    auto jit = CajetaJit::compile(kProbe, "test.D", o);
    if (!jit) return -99;
    auto fn = jit->lookup<int32_t (*)()>("run");
    if (!fn) return -98;
    return fn();
}

} // namespace

// --debug-info=full keeps the marks, so a frame resolves to an exact line.
// This is the half that would break if the gate were placed wrongly.
TEST(LineMarkGranularity, fullDebugInfoResolvesAnExactLine) {
    const int32_t line = frameLineUnder(/*fullDebugInfo=*/true);
    ASSERT_GT(line, 0)
        << "no exact line under --debug-info=full (got " << line << ")";
}

// The default keeps per-call enter/leave — so the trace still names the type,
// method and file — but carries no line. StackFrame documents 0 as "line-info
// unavailable", which is exactly the state this build is in.
//
// Asserted as == 0 rather than "not equal to the full answer": a frame that
// reported some other line would be worse than one reporting none, because a
// wrong line reads exactly like a right one.
TEST(LineMarkGranularity, defaultBuildResolvesTheFrameButNotTheLine) {
    const int32_t line = frameLineUnder(/*fullDebugInfo=*/false);
    EXPECT_EQ(line, 0)
        << "default build produced line " << line
        << "; per-statement marks are supposed to be a --debug-info=full "
           "feature now (see LineInfoCodegen::emitLineMark)";
}

// The frame itself must survive in the default build — type, method and file
// come from the per-call enter, which stays on. If this regressed, the default
// would have lost stack traces entirely rather than just their line numbers,
// and the test above would still pass.
TEST(LineMarkGranularity, defaultBuildStillNamesTheFrame) {
    CajetaJit::Options o;
    o.debugInfoEnabled = false;
    auto jit = CajetaJit::compile(
        "package test;\n"
        "import cajeta.error.Exception;\n"
        "import cajeta.error.StackFrame;\n"
        "public final class D {\n"
        "    public static void boom() { throw heap Exception(\"x\"); }\n"
        "    public static int32 run() {\n"
        "        try {\n"
        "            D.boom();\n"
        "            return -1;\n"
        "        } catch (Exception e) {\n"
        "            StackFrame[] fs = e.getStackTrace();\n"
        "            if (fs.count() == 0) { return -2; }\n"
        "            if (fs[0].method.byteLength() == 0) { return -3; }\n"
        "            if (fs[0].file.byteLength() == 0) { return -4; }\n"
        "            return 1;\n"
        "        }\n"
        "    }\n"
        "}\n", "test.D", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 1) << "the default build lost the frame, not just its line";
}
