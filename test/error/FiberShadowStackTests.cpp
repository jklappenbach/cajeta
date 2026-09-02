// cajeta-profiler Unit 2 — per-fiber shadow stack (plan 2.1, spec §4.1/§4.2/§4.4/§4.5).
//
// The defect: a fiber's __cajeta_line_enter/mark/leave run on the CARRIER
// THREAD's TLS shadow stack (cajeta_rt_core.c), so frames from different fibers
// hosted by one carrier interleave into a single stack, and a yield leaves stale
// entries behind. The debug frame chain already solved this with a per-fiber slot
// (__cajeta_dbg_top_ptr); the shadow stack has no equivalent.
//
// Everything here is observed IN-LANGUAGE through getStackTrace(), because that
// is the only consumer of the shadow stack a test can reach: calling
// __cajeta_shadow_get_top() from the test process would read the NATIVE runtime
// copy's TLS, not the JIT copy's.
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include <cstdint>
#include <string>
using cajeta_test::CajetaJit;

namespace {
int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    if (!fn) return -1;
    return fn();
}
} // namespace

// 2.1.e — CONTROL, main program thread. Must pass BEFORE the fix as well as
// after: this is the "unaffected" half of the unit, and if it ever fails the
// per-fiber work has regressed the ordinary path.
TEST(FiberShadowStack, mainThreadTraceResolves) {
    EXPECT_GT(runI32(
        "package test;\n"
        "import cajeta.error.Exception;\n"
        "import cajeta.error.StackFrame;\n"
        "public final class D {\n"
        "    public static void deep() { throw heap Exception(\"boom\"); }\n"
        "    public static int32 run() {\n"
        "        try {\n"
        "            D.deep();\n"
        "            return 0;\n"
        "        } catch (Exception e) {\n"
        "            StackFrame[] fs #= e.getStackTrace();\n"
        "            return fs.count();\n"
        "        }\n"
        "    }\n"
        "}\n"), 0);
}

// 2.1.d — the existing defect. A throw inside a fiber must resolve to that
// fiber's frames. Expected RED before the fix: __cajeta_trace_record returns
// early when __cajeta_current_fiber is set, so the trace is empty.
TEST(FiberShadowStack, fiberTraceResolves) {
    EXPECT_GT(runI32(
        "package test;\n"
        "import cajeta.error.Exception;\n"
        "import cajeta.error.StackFrame;\n"
        "public final class D {\n"
        "    public static void deep() { throw heap Exception(\"boom\"); }\n"
        "    public static async int32 inFiber() {\n"
        "        try {\n"
        "            D.deep();\n"
        "            return 0;\n"
        "        } catch (Exception e) {\n"
        "            StackFrame[] fs #= e.getStackTrace();\n"
        "            return fs.count();\n"
        "        }\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Task<int32> t = spawn inFiber();\n"
        "        return await t;\n"
        "    }\n"
        "}\n"), 0);
}

// 2.1.a — two fibers interleaving on one carrier each observe THEIR OWN depth.
//
// Asserted differentially: `deepFiber` throws through d1->d2->d3 and
// `shallowFiber` throws straight from d3, so the deep trace must carry exactly 2
// more frames than the shallow one — whatever common frames sit underneath. A
// shared carrier-TLS stack contaminates one fiber's depth with the other's and
// the difference stops being 2, which is the defect this unit fixes.
TEST(FiberShadowStack, twoFibersKeepOwnDepth) {
    EXPECT_EQ(runI32(
        "package test;\n"
        "import cajeta.error.Exception;\n"
        "import cajeta.error.StackFrame;\n"
        "public final class D {\n"
        "    public static void d3() { throw heap Exception(\"boom\"); }\n"
        "    public static void d2() { D.d3(); }\n"
        "    public static void d1() { D.d2(); }\n"
        "    public static async int32 deepFiber() {\n"
        "        try { D.d1(); return 0; }\n"
        "        catch (Exception e) { StackFrame[] fs #= e.getStackTrace(); return fs.count(); }\n"
        "    }\n"
        "    public static async int32 shallowFiber() {\n"
        "        try { D.d3(); return 0; }\n"
        "        catch (Exception e) { StackFrame[] fs #= e.getStackTrace(); return fs.count(); }\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Task<int32> a = spawn deepFiber();\n"
        "        Task<int32> b = spawn shallowFiber();\n"
        "        int32 va = await a;\n"
        "        int32 vb = await b;\n"
        "        return va - vb;\n"
        "    }\n"
        "}\n"), 2);
}

// 2.1.b — a fiber that yields mid-call and resumes sees its frames intact.
//
// Differential again: the same throw shape, once with an await (a real yield)
// before it and once without. A yield that leaves stale entries or loses the
// fiber's frames changes the count; identical shapes must produce identical
// depth, so the difference is 0.
//
// The zero-guards are load-bearing. Without them this passed VACUOUSLY against
// the unfixed runtime: both fibers reported 0 frames, and 0 - 0 == 0. A check
// needs a test that asserts it FIRES, not one a broken runtime satisfies by
// returning nothing (CLAUDE.md §5).
TEST(FiberShadowStack, yieldPreservesFrames) {
    EXPECT_EQ(runI32(
        "package test;\n"
        "import cajeta.error.Exception;\n"
        "import cajeta.error.StackFrame;\n"
        "public final class D {\n"
        "    public static void d2() { throw heap Exception(\"boom\"); }\n"
        "    public static void d1() { D.d2(); }\n"
        "    public static async int32 quiet() { return 1; }\n"
        "    public static async int32 withYield() {\n"
        "        try {\n"
        "            Task<int32> q = spawn quiet();\n"
        "            int32 v = await q;\n"
        "            D.d1();\n"
        "            return 0;\n"
        "        } catch (Exception e) { StackFrame[] fs #= e.getStackTrace(); return fs.count(); }\n"
        "    }\n"
        "    public static async int32 noYield() {\n"
        "        try { D.d1(); return 0; }\n"
        "        catch (Exception e) { StackFrame[] fs #= e.getStackTrace(); return fs.count(); }\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Task<int32> a = spawn withYield();\n"
        "        Task<int32> b = spawn noYield();\n"
        "        int32 va = await a;\n"
        "        int32 vb = await b;\n"
        "        if (va <= 0) { return -1; }\n"
        "        if (vb <= 0) { return -2; }\n"
        "        return va - vb;\n"
        "    }\n"
        "}\n"), 0);
}

// 2.1.c — attribution survives fibers being spread across carrier threads.
//
// Migration cannot be forced from the language surface, so this spawns six
// identically-shaped fibers and asserts every one reports the SAME depth. Frames
// leaking between fibers — the shared-TLS defect — makes the later fibers deeper
// than the first. Best-effort on scheduling, exact on the invariant: sameness is
// true whether or not the scheduler actually migrated anything, so the test never
// flakes, it just covers more when it does.
TEST(FiberShadowStack, manyFibersAgreeOnDepth) {
    EXPECT_EQ(runI32(
        "package test;\n"
        "import cajeta.error.Exception;\n"
        "import cajeta.error.StackFrame;\n"
        "public final class D {\n"
        "    public static void d2() { throw heap Exception(\"boom\"); }\n"
        "    public static void d1() { D.d2(); }\n"
        "    public static async int32 one() {\n"
        "        try { D.d1(); return 0; }\n"
        "        catch (Exception e) { StackFrame[] fs #= e.getStackTrace(); return fs.count(); }\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Task<int32> t1 = spawn one();\n"
        "        Task<int32> t2 = spawn one();\n"
        "        Task<int32> t3 = spawn one();\n"
        "        Task<int32> t4 = spawn one();\n"
        "        Task<int32> t5 = spawn one();\n"
        "        Task<int32> t6 = spawn one();\n"
        "        int32 v1 = await t1;\n"
        "        int32 v2 = await t2;\n"
        "        int32 v3 = await t3;\n"
        "        int32 v4 = await t4;\n"
        "        int32 v5 = await t5;\n"
        "        int32 v6 = await t6;\n"
        "        if (v1 <= 0) { return -1; }\n"
        "        int32 spread = 0;\n"
        "        if (v2 != v1) { spread = spread + 1; }\n"
        "        if (v3 != v1) { spread = spread + 1; }\n"
        "        if (v4 != v1) { spread = spread + 1; }\n"
        "        if (v5 != v1) { spread = spread + 1; }\n"
        "        if (v6 != v1) { spread = spread + 1; }\n"
        "        return spread;\n"
        "    }\n"
        "}\n"), 0);
}
