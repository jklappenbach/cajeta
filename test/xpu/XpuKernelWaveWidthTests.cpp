//
// datacenter-gpus Unit 3 (specs/datacenter-gpus-spec.md §2.3): `k.waveWidth()` on the
// host answers the width k's loaded device code was built at, so a launcher sizes its
// block in waves. Decided with Julian 2026-11-17: a kernel with no @Wave builds at the
// target's native width, which is 64 on CDNA, so a launcher can no longer assume 32.
//
// The width it answers must be the one Wave.width() reads inside the kernel: each test
// launches the kernel, reads Wave.width() back, and compares it to the host query. A
// template kernel is asked by its bare name, since the launcher (`run<E>`) has no
// type arguments to give it.
//

#include <gtest/gtest.h>
#include "XpuDeviceTestUtil.h"
#include "KernelLoweringProbe.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/xpu/XpuTarget.h"
#include "cajeta/xpu/core/KernelManifest.h"
#include "cajeta/xpu/amd/AmdgpuBackend.h"
#include "cajeta/xpu/amd/AmdgpuRegistration.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

const char* kSource = R"CJ(
package test;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
import cajeta.xpu.KernelThread;
import cajeta.xpu.Wave;
public class W {
    @Kernel
    public static void plain(KernelBuffer<int32> y) {
        if (KernelThread.x() == 0) { y[0] = (int32) Wave.width(); }
    }
    @Kernel @Wave(width = 32)
    public static void pinned(KernelBuffer<int32> y) {
        if (KernelThread.x() == 0) { y[0] = (int32) Wave.width(); }
    }
    @Kernel
    public static void tmpl<E>(KernelBuffer<E> y) {
        if (KernelThread.x() == 0) { y[0] = (E) Wave.width(); }
    }
    static int32 launched(int32 which) {
        KernelBuffer<int32> y = heap KernelBuffer<int32>(1);
        KernelStream s #= KernelStream.current();
        if (which == 0) { plain.launch(s, grid: [1], block: [64])(y); }
        if (which == 1) { pinned.launch(s, grid: [1], block: [64])(y); }
        if (which == 2) { tmpl.launch(s, grid: [1], block: [64])(y); }
        s.sync();
        int32[] h = heap int32[1];
        y.download(h);
        return h[0];
    }
    public static int32 devPlain() { return W.launched(0); }
    public static int32 devPinned() { return W.launched(1); }
    public static int32 devTmpl() { return W.launched(2); }
    public static int32 hostPlain() { return plain.waveWidth(); }
    public static int32 hostPinned() { return pinned.waveWidth(); }
    public static int32 hostTmpl() { return tmpl.waveWidth(); }
}
)CJ";

void expectHostMatchesDevice(cajeta::xpu::Backend backend) {
    CajetaJit::Options o;
    o.xpuBackends = {backend};
    auto jit = CajetaJit::compile(kSource, "test.W", o);
    ASSERT_TRUE(jit);
    for (const char* k : {"Plain", "Pinned", "Tmpl"}) {
        auto host = jit->lookup<int32_t (*)()>(std::string("host") + k);
        auto dev = jit->lookup<int32_t (*)()>(std::string("dev") + k);
        ASSERT_TRUE(host && dev) << k;
        int32_t h = host();
        int32_t d = dev();
        EXPECT_GT(h, 0) << k << ": no width for a loaded kernel";
        EXPECT_EQ(d, h) << k << ": the host query disagrees with Wave.width() in the kernel";
        if (std::string(k) == "Pinned") EXPECT_EQ(32, h) << "a pinned kernel answers its pin";
        // The template is asked after its instantiation launched, and before it.
        if (std::string(k) == "Tmpl") EXPECT_EQ(h, host());
    }
}

} // namespace

// The cpu backend builds a kernel at its own width; the query must agree with it.
TEST(XpuKernelWaveWidth, theHostQueryMatchesTheKernelOnCpu) {
    expectHostMatchesDevice(cajeta::xpu::Backend::Cpu);
}

TEST(XpuKernelWaveWidthDeviceTests, theHostQueryMatchesTheKernelOnCuda) {
    if (!cajeta::xpu::test::cudaAvailable()) GTEST_SKIP() << "no CUDA device";
    expectHostMatchesDevice(cajeta::xpu::Backend::Nvptx);
}

TEST(XpuKernelWaveWidthDeviceTests, theHostQueryMatchesTheKernelOnHip) {
    if (!cajeta::xpu::test::hipAvailable()) GTEST_SKIP() << "no HIP device";
    expectHostMatchesDevice(cajeta::xpu::Backend::Amdgpu);
}

// What a gfx942 build registers for the unpinned kernel, offline: the width the host
// query reads there.
TEST(XpuKernelWaveWidth, aGfx942BuildRegistersWave64) {
    // Registration embeds an assembled code object, which needs ld.lld.
    if (cajeta::xpu::amd::findLld().empty()) GTEST_SKIP() << "no ld.lld to assemble a gfx942 code object";
    cajeta::Compiler compiler;
    auto module = cajeta::xpu::probe::compileForInspection(compiler, kSource, "test.W", "wavewidth");
    auto k = cajeta::xpu::probe::findMethod(module->getStructures()["test.W"], "plain");
    ASSERT_TRUE(k);
    llvm::LLVMContext ctx;
    llvm::Module host("wavewidth_host", ctx);
    std::vector<cajeta::xpu::KernelManifest> out;
    cajeta::xpu::amd::emitKernelRegistration({k}, host, "gfx942", {}, &out);
    ASSERT_EQ(1u, out.size());
    ASSERT_TRUE(out[0].waveWidth.has_value());
    EXPECT_EQ(64u, *out[0].waveWidth);
}

// A name that is not a @Kernel of the class is a compile error, as for launch.
TEST(XpuKernelWaveWidth, aNonKernelReceiverIsRefused) {
    const char* src = R"CJ(
package test;
public class N {
    static void helper() { return; }
    public static int32 run() { return helper.waveWidth(); }
}
)CJ";
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    std::string err;
    try {
        auto jit = CajetaJit::compile(src, "test.N", o);
        EXPECT_FALSE(jit) << "helper.waveWidth() on a non-kernel must not compile";
    } catch (cajeta::Exception& e) {
        err = e.getMessage();
    } catch (const std::exception& e) {
        err = e.what();
    }
    EXPECT_NE(err.find("helper"), std::string::npos) << err;
}


// A wave-generic kernel divides by Wave.width(), so on nvptx, where every warp is 32,
// the width must be the constant 32 from the start. A %WARP_SZ read folds only late in
// the NVPTX backend: the optimizer saw a runtime value, simplified t / width differently,
// and gemm256 at f16 spilled 8 bytes where it spilled 4, 5% slower on the 4090
// (1868 against 1773 us at n4096, interleaved). The check is on the device IR.
TEST(XpuKernelWaveWidth, waveWidthIsTheConstant32OnNvptx) {
    auto l = cajeta::xpu::probe::lowerForNvptx(kSource, "plain", "test.W", "sm_89", "wavewidth");
    ASSERT_TRUE(l.ok) << l.why;
    EXPECT_EQ(l.ir.find("llvm.nvvm.read.ptx.sreg.warpsize"), std::string::npos) << l.ir;
}
