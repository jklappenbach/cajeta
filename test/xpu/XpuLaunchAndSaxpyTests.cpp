//
// Steps 6 + 7 (minimal-shipping form) — kernel launch surface and
// the CPU-emulation SAXPY milestone.
//
// The CajetaXPU.md spec calls for a `saxpy.launch(stream, grid:,
// block:)(args)` syntax that requires a non-trivial expression-
// grammar extension (a postfix `(args)` after an expression result).
// That grammar work is deferred — see the plan's "Open decisions"
// section. For phase 1+2 CPU emulation, the @Kernel body itself is
// a perfectly callable static method; the "launch" abstraction
// degenerates to a host-side loop calling the kernel once per
// thread index.
//
// What this file ships for step 6:
//   - KernelArg admissibility now includes primitive arrays
//     (`float32[]`, `int32[]`, …). Recursively admissible — arrays
//     of admissible element types are also admissible.
//
// What this file ships for step 7:
//   - End-to-end SAXPY: a @Kernel runs from JIT-compiled Cajeta
//     source, host code iterates over indices, output array
//     matches the CPU-computed reference. The SAXPY kernel body is
//     Cajeta source, not C; the host driver is also Cajeta source.
//     The C++ test boundary just checks the final sum.
//
// Real launch-site recognition + deferred-borrow tracking lands
// when the grammar work for the launch postfix is complete; that
// then enables stream/event-based scheduling on the NVPTX path.
//

#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"

using cajeta_test::CajetaJit;

// Step 6: primitive arrays admissible as @Kernel parameters. The
// validator extension means a kernel like `saxpyOne(uint32 i,
// float32[] y, float32[] x, float32 a, uint32 n)` compiles where
// previously the `float32[]` parameters were rejected.
TEST(XpuLaunchTests, primitiveArrayParameterAdmissible) {
    auto src =
        "package test;\n"
        "public class K {\n"
        "    @Kernel\n"
        "    public static void run(uint32 i, float32[] y, uint32 n) {\n"
        "        if (i < n) {\n"
        "            y[i] = y[i] + 1.0f;\n"
        "        }\n"
        "    }\n"
        "    public static int32 dummy() { return 0; }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.K");
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int32_t (*)()>("dummy");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 0);  // smoke — kernel-defining class must JIT
}

// Step 7 milestone: SAXPY runs end-to-end on the CPU-emulation
// path. The kernel is annotated @Kernel; the host driver explicitly
// iterates over the index space and calls the kernel each step. On
// the NVPTX backend (step 11) the same source compiles into a real
// device launch — that's the variance-discipline payoff. The
// arithmetic checked here matches the canonical SAXPY identity:
// y[i] := a * x[i] + y[i].
TEST(XpuLaunchTests, saxpyCpuEmulationEndToEnd) {
    auto src =
        "package test;\n"
        "public class Saxpy {\n"
        "    @Kernel\n"
        "    public static void saxpyOne(uint32 i,\n"
        "                                float32[] y,\n"
        "                                float32[] x,\n"
        "                                float32 a,\n"
        "                                uint32 n) {\n"
        "        if (i < n) {\n"
        "            y[i] = a * x[i] + y[i];\n"
        "        }\n"
        "    }\n"
        "    public static float32 run() {\n"
        "        uint32 n = 8;\n"
        "        float32[] x = heap float32[n];\n"
        "        float32[] y = heap float32[n];\n"
        "        for (uint32 i = 0; i < n; i = i + 1) {\n"
        "            x[i] = 1.0f;\n"
        "            y[i] = 2.0f;\n"
        "        }\n"
        "        for (uint32 i = 0; i < n; i = i + 1) {\n"
        "            saxpyOne(i, y, x, 2.0f, n);\n"
        "        }\n"
        "        float32 sum = 0.0f;\n"
        "        for (uint32 i = 0; i < n; i = i + 1) {\n"
        "            sum = sum + y[i];\n"
        "        }\n"
        "        return sum;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.Saxpy");
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<float (*)()>("run");
    ASSERT_NE(fn, nullptr);
    // Each y[i] = 2.0 * 1.0 + 2.0 = 4.0; sum over 8 elements = 32.0
    EXPECT_FLOAT_EQ(fn(), 32.0f);
}
