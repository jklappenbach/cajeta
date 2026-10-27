//
// AmdgpuWordFormTests — byte vectors on AMDGPU are wide loads WITHOUT a proof
// (kernel-load-alignment plan Unit 2, closed as measured 2026-10-27).
//
// The plan expected gfx1151 to split an int8 `vload<16>` at alignment 1 into
// byte loads, as NVPTX did before NvptxWordFormTests. It does not: the AMDGPU
// backend runs in unaligned-access mode and issues one `global_load_b128`
// whether or not the index is aligned. Across cajeta-llm's 314 AMDGPU kernels
// there was no v_perm/v_alignbyte realignment at all. So AMDGPU takes no raise
// and owes no aligned base at launch (no `cajeta-word-aligned-params`), and a
// launch on any base runs. These pin that: if a backend change ever splits
// these loads, the proof belongs here after all.
//
#include <gtest/gtest.h>
#include "KernelLoweringProbe.h"

#include <string>

using namespace cajeta::xpu::probe;

namespace {

const char* kSource =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void aligned(KernelBuffer<int32> out,\n"
    "            KernelBuffer<int8> w, KernelBuffer<int8> x, uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) {\n"
    "            Vector<int32,4> zv = heap Vector<int32,4>(0, 0, 0, 0);\n"
    "            Vector<uint8,16> wv = w.vload<16>((int64) i * 16L).asUnsigned();\n"
    "            Vector<int8,16> xv = x.vload<16>((int64) i * 16L);\n"
    "            Vector<int32,4> t = wv.dotAccum(xv, zv);\n"
    "            out[(int64) i] = t[0] + t[1] + t[2] + t[3];\n"
    "        }\n"
    "    }\n"
    "    @Kernel\n"
    "    public static void loose(KernelBuffer<int32> out,\n"
    "            KernelBuffer<int8> w, KernelBuffer<int8> x, uint32 n, uint32 off) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) {\n"
    "            Vector<int32,4> zv = heap Vector<int32,4>(0, 0, 0, 0);\n"
    "            Vector<uint8,16> wv = w.vload<16>((int64) (i + off)).asUnsigned();\n"
    "            Vector<int8,16> xv = x.vload<16>((int64) (i + off));\n"
    "            Vector<int32,4> t = wv.dotAccum(xv, zv);\n"
    "            out[(int64) i] = t[0] + t[1] + t[2] + t[3];\n"
    "        }\n"
    "    }\n"
    "    public static int32 run() { return 1; }\n"
    "}\n";

Lowered lower(const char* kernel) {
    return lowerForAmdgpu(kSource, kernel, "test.M", "gfx1151", "amdwordform");
}

std::size_t byteLoads(const std::string& isa) {
    return countOf(isa, "global_load_u8") + countOf(isa, "global_load_i8")
         + countOf(isa, "global_load_ubyte") + countOf(isa, "global_load_sbyte");
}

std::size_t wideLoads(const std::string& isa) {
    return countOf(isa, "global_load_b128") + countOf(isa, "global_load_dwordx4");
}

}  // namespace

// An index of i * 16: one 128-bit load per operand, no byte loads.
TEST(AmdgpuWordForm, anIntEightVloadAtAnAlignedIndexIsOneWideLoad) {
    const Lowered l = lower("aligned");
    ASSERT_TRUE(l.ok) << l.why;
    EXPECT_EQ(byteLoads(l.ptx), 0u) << l.ptx;
    EXPECT_GE(wideLoads(l.ptx), 2u) << l.ptx;
}

// An index nothing proves aligned is ALSO one 128-bit load (unaligned-access
// mode), and the kernel demands nothing of the launch's base.
TEST(AmdgpuWordForm, anUnprovableIndexIsAlsoOneWideLoadAndOwesNoBase) {
    const Lowered l = lower("loose");
    ASSERT_TRUE(l.ok) << l.why;
    EXPECT_EQ(byteLoads(l.ptx), 0u)
        << "gfx1151 now splits an unaligned int8 vload: the shared proof "
           "should run on AMDGPU (kernel-load-alignment Unit 2)\n" << l.ptx;
    EXPECT_GE(wideLoads(l.ptx), 2u) << l.ptx;
    EXPECT_EQ(l.ir.find("cajeta-word-aligned-params"), std::string::npos) << l.ir;
}
