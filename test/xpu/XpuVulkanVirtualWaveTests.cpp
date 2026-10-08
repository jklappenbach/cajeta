// vulkan-virtual-waves: a declared @Wave(width) runs on a Vulkan device whose subgroup cannot be
// pinned to it, and matches the cpu (the reference order at that width) bit for bit.
#include "gtest/gtest.h"
#include "../PortableEnv.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/xpu/XpuTarget.h"
#include "cajeta/xpu/vulkan/VulkanDriver.h"
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using cajeta_test::CajetaJit;

namespace {

const uint32_t kN = 768;

const char* kSource = R"CJ(
package test;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
import cajeta.xpu.KernelThread;
import cajeta.xpu.Wave;
public class V {
    static float32[] got32;
    static float32[] got64;
    @Kernel
    @Wave(width = 32)
    public static void vw32(KernelBuffer<float32> out, KernelBuffer<float32> in) {
        uint32 g = KernelThread.globalIdX();
        float32 v = in[g];
        uint32 lane = Wave.laneId();
        out[g * 4] = Wave.reduceSumF32(v);
        float32 s = -1.0f;
        if (lane < 16) {
            s = Wave.shuffleXorSyncF32(v, 3);
        }
        out[g * 4 + 1] = s;
        float32 t = -2.0f;
        if ((lane & 1) == 0) {
            t = Wave.shuffleXorSyncF32(v, 10);
        }
        out[g * 4 + 2] = t;
        out[g * 4 + 3] = (float32) lane;
    }
    @Kernel
    @Wave(width = 64)
    public static void vw64(KernelBuffer<float32> out, KernelBuffer<float32> in) {
        uint32 g = KernelThread.globalIdX();
        float32 v = in[g];
        uint32 lane = Wave.laneId();
        out[g * 4] = Wave.reduceSumF32(v);
        float32 s = -1.0f;
        if (lane < 40) {
            s = Wave.shuffleXorSyncF32(v, 33);
        }
        out[g * 4 + 1] = s;
        float32 t = -2.0f;
        if ((lane & 1) == 0) {
            t = Wave.shuffleXorSyncF32(v, 18);
        }
        out[g * 4 + 2] = t;
        out[g * 4 + 3] = (float32) lane;
    }
    public static int32 run() {
        uint32 n = 768;
        float32[] h = heap float32[n];
        for (uint32 i = 0; i < n; i = i + 1) {
            h[i] = (float32) i * 0.3701f - 19.3f;
        }
        KernelBuffer<float32> in = heap KernelBuffer<float32>(n);
        KernelBuffer<float32> out32 = heap KernelBuffer<float32>(n * 4);
        KernelBuffer<float32> out64 = heap KernelBuffer<float32>(n * 4);
        in.upload(h);
        KernelStream s #= KernelStream.current();
        vw32.launch(s, grid: [3], block: [256])(out32, in);
        s.sync();
        vw64.launch(s, grid: [3], block: [256])(out64, in);
        s.sync();
        got32 = heap float32[n * 4];
        got64 = heap float32[n * 4];
        out32.download(got32);
        out64.download(got64);
        return 1;
    }
    public static float32 a(uint32 i) { return got32[i]; }
    public static float32 b(uint32 i) { return got64[i]; }
}
)CJ";

struct Outputs {
    std::vector<float> a, b;
    std::string stderrText, compileText;
    bool ran = false;
};

Outputs runOn(cajeta::xpu::Backend be) {
    Outputs o;
    CajetaJit::Options opts;
    opts.xpuBackends = {be};
    auto jit = CajetaJit::compile(kSource, "test.V", opts);
    EXPECT_NE(jit, nullptr);
    if (!jit) return o;
    auto fn = jit->lookup<int32_t (*)()>("run");
    auto aAt = jit->lookup<float (*)(uint32_t)>("a");
    auto bAt = jit->lookup<float (*)(uint32_t)>("b");
    EXPECT_TRUE(fn && aAt && bAt);
    if (!(fn && aAt && bAt)) return o;
    testing::internal::CaptureStderr();
    int32_t r = fn();
    o.stderrText = testing::internal::GetCapturedStderr();
    EXPECT_EQ(r, 1);
    for (uint32_t k = 0; k < kN * 4; ++k) {
        o.a.push_back(aAt(k));
        o.b.push_back(bAt(k));
    }
    o.ran = true;
    return o;
}

uint32_t bits(float v) { uint32_t b; std::memcpy(&b, &v, 4); return b; }

void sameBits(const std::vector<float>& dev, const std::vector<float>& cpu, const char* what) {
    ASSERT_EQ(dev.size(), cpu.size());
    for (size_t k = 0; k < cpu.size(); ++k)
        ASSERT_EQ(bits(dev[k]), bits(cpu[k])) << what << "[" << k << "] (field " << k % 4
            << "): " << dev[k] << " vs cpu " << cpu[k];
}

}  // namespace

// 1.1.1, 1.1.2: on a device that cannot pin 32 or 64, both kernels run as virtual waves and
// match the cpu bit for bit: the float reduce, a shuffle under a slot-divergent branch, one
// under a lane-parity branch, and the logical lane id.
TEST(XpuVulkanVirtualWave, aWaveTheDeviceCannotPinRunsVirtuallyAndMatchesTheCpu) {
    if (cajeta::xpu::vulkan::VulkanDriver::canRunSubgroupWidth(32)
            && cajeta::xpu::vulkan::VulkanDriver::canRunSubgroupWidth(64))
        GTEST_SKIP() << "this Vulkan device pins 32 and 64, so nothing runs virtually";
    Outputs dev = runOn(cajeta::xpu::Backend::Spirv);
    ASSERT_TRUE(dev.ran);
    EXPECT_EQ(dev.stderrText.find("xpu-launch-refused"), std::string::npos) << dev.stderrText;
    EXPECT_EQ(dev.stderrText.find("[xpu-kernel-skipped]"), std::string::npos) << dev.stderrText;
    Outputs cpu = runOn(cajeta::xpu::Backend::Cpu);
    ASSERT_TRUE(cpu.ran);
    sameBits(dev.a, cpu.a, "vw32");
    sameBits(dev.b, cpu.b, "vw64");
}

namespace {

const char* kLoopSource = R"CJ(
package test;
import cajeta.xpu.Barrier;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
import cajeta.xpu.KernelThread;
import cajeta.xpu.Wave;
public class L {
    static float32[] got;
    @Kernel
    @Wave(width = 32)
    public static void vary(KernelBuffer<float32> out, KernelBuffer<float32> in) {
        uint32 g = KernelThread.globalIdX();
        uint32 lane = Wave.laneId();
        float32 acc = 0.0f;
        uint32 j = lane;
        while (j < 70) {
            acc = acc + in[j];
            j = j + 32;
        }
        out[g * 3] = Wave.reduceSumF32(acc);
    }
    @Kernel
    @Wave(width = 32)
    public static void uniform(KernelBuffer<float32> out, KernelBuffer<float32> in) {
        uint32 g = KernelThread.globalIdX();
        float32 acc = 0.0f;
        uint32 j = 0;
        while (j < 3) {
            acc = acc + Wave.reduceSumF32(in[g] * (float32) (j + 1));
            j = j + 1;
        }
        out[g * 3 + 1] = acc;
    }
    @Kernel
    @Wave(width = 32)
    public static void barrier(KernelBuffer<float32> out, KernelBuffer<float32> in) {
        uint32 g = KernelThread.globalIdX();
        float32 r = Wave.reduceSumF32(in[g]);
        Barrier.workgroup();
        out[g * 3 + 2] = r + in[(g + 32) % 256];
    }
    public static int32 run() {
        uint32 n = 256;
        float32[] h = heap float32[n];
        for (uint32 i = 0; i < n; i = i + 1) {
            h[i] = (float32) i * 0.4219f - 31.7f;
        }
        KernelBuffer<float32> in = heap KernelBuffer<float32>(n);
        KernelBuffer<float32> out = heap KernelBuffer<float32>(n * 3);
        in.upload(h);
        KernelStream s #= KernelStream.current();
        vary.launch(s, grid: [2], block: [128])(out, in);
        s.sync();
        uniform.launch(s, grid: [2], block: [128])(out, in);
        s.sync();
        barrier.launch(s, grid: [2], block: [128])(out, in);
        s.sync();
        got = heap float32[n * 3];
        out.download(got);
        return 1;
    }
    public static float32 at(uint32 i) { return got[i]; }
}
)CJ";

Outputs runLoopsOn(cajeta::xpu::Backend be) {
    Outputs o;
    CajetaJit::Options opts;
    opts.xpuBackends = {be};
    testing::internal::CaptureStderr();
    auto jit = CajetaJit::compile(kLoopSource, "test.L", opts);
    o.compileText = testing::internal::GetCapturedStderr();
    EXPECT_NE(jit, nullptr);
    if (!jit) return o;
    auto fn = jit->lookup<int32_t (*)()>("run");
    auto at = jit->lookup<float (*)(uint32_t)>("at");
    EXPECT_TRUE(fn && at);
    if (!(fn && at)) return o;
    testing::internal::CaptureStderr();
    int32_t r = fn();
    o.stderrText = testing::internal::GetCapturedStderr();
    EXPECT_EQ(r, 1);
    for (uint32_t k = 0; k < 256 * 3; ++k) o.a.push_back(at(k));
    o.ran = true;
    return o;
}

}  // namespace

// 1.1.4: a lane-varying loop before a reduce, a reduce in a uniform loop, and a reduce
// before a workgroup barrier run virtually and match the cpu bit for bit.
TEST(XpuVulkanVirtualWave, loopsAndBarriersRunVirtuallyAndMatchTheCpu) {
    if (cajeta::xpu::vulkan::VulkanDriver::canRunSubgroupWidth(32))
        GTEST_SKIP() << "this Vulkan device pins 32, so nothing runs virtually";
    Outputs dev = runLoopsOn(cajeta::xpu::Backend::Spirv);
    ASSERT_TRUE(dev.ran);
    EXPECT_EQ(dev.compileText.find("skipped]"), std::string::npos) << dev.compileText;
    EXPECT_EQ(dev.stderrText.find("xpu-launch-refused"), std::string::npos) << dev.stderrText;
    Outputs cpu = runLoopsOn(cajeta::xpu::Backend::Cpu);
    ASSERT_TRUE(cpu.ran);
    sameBits(dev.a, cpu.a, "loops");
}

namespace {

const uint32_t kVN = 512;

const char* kVerbSource = R"CJ(
package test;
import cajeta.xpu.Group;
import cajeta.xpu.GroupOp;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
import cajeta.xpu.KernelThread;
import cajeta.xpu.Quad;
import cajeta.xpu.Wave;
import cajeta.xpu.Workgroup;
public class G {
    static uint32[] gotU;
    static float32[] gotF;
    static float32[] gotG;
    static int32[] gotW;
    static uint32[] gotD;
    @Kernel
    @Wave(width = 32)
    public static void verbs(KernelBuffer<uint32> outU, KernelBuffer<float32> outF,
                             KernelBuffer<uint32> inU, KernelBuffer<float32> inF) {
        uint32 g = KernelThread.globalIdX();
        uint32 u = inU[g];
        float32 f = inF[g];
        uint32 lane = Wave.laneId();
        uint32 b = g * 20;
        outU[b] = Wave.shuffleSync(u, (lane * 7) % 32);
        outU[b + 1] = Wave.shuffleUpSync(u, 3);
        outU[b + 2] = Wave.shuffleDownSync(u, 5);
        outU[b + 3] = Wave.rotate(u, 9);
        uint64 bal = Wave.ballotSync((u & 1) == 1);
        uint64 balHi = bal >> 32;
        outU[b + 4] = (uint32) (bal - (balHi << 32));
        outU[b + 5] = (uint32) balHi;
        outU[b + 6] = Wave.reduceSum(u);
        outU[b + 7] = Wave.reduceMax(u);
        outU[b + 8] = Wave.reduceMin(u);
        outU[b + 9] = Wave.reduceAnd(u | 4096);
        outU[b + 10] = Wave.reduceOr(u);
        outU[b + 11] = Wave.reduceXor(u);
        outU[b + 12] = Wave.prefixSum(u);
        outU[b + 13] = Wave.prefixProduct((u & 1) + 1);
        outU[b + 14] = Quad.broadcast(u, 2);
        outU[b + 15] = Quad.swapDiagonal(u);
        uint32 q = 0;
        if (Quad.any((u & 3) == 0)) { q = 1; }
        if (Quad.all((u & 1) == 1)) { q = q + 2; }
        outU[b + 16] = q;
        outU[b + 18] = 0;
        uint32 d = 77;
        if (lane % 3 != 0) {
            d = Wave.reduceSum(u) + Wave.prefixSum(u);
            uint64 db = Wave.ballotSync((u & 2) == 2);
            uint64 dbHi = db >> 32;
            outU[b + 18] = (uint32) (db - (dbHi << 32));
        }
        outU[b + 17] = d;
        outU[b + 19] = Wave.width();
        uint32 c = g * 6;
        outF[c] = Wave.reduceSumF32(f);
        outF[c + 1] = Wave.reduceMaxF32(f);
        outF[c + 2] = Wave.reduceSumF32Segmented(f, 8);
        outF[c + 3] = Wave.reduceMaxF32Segmented(f, 4);
        outF[c + 4] = Wave.shuffleDownSyncF32(f, 1);
        float32 e = -5.0f;
        if (lane >= 9) {
            e = Wave.reduceSumF32(f);
        }
        outF[c + 5] = e;
    }
    @Kernel
    @Wave(width = 32)
    public static void grp(KernelBuffer<float32> out, KernelBuffer<float32> inF) {
        uint32 g = KernelThread.globalIdX();
        float32 f = inF[g];
        out[g * 4] = Group.reduce(GroupOp.Add, f);
        out[g * 4 + 1] = Group.reduce(GroupOp.Max, f);
        out[g * 4 + 2] = (float32) Group.laneId();
        out[g * 4 + 3] = (float32) Group.width();
    }
    @Kernel
    @Wave(width = 32)
    public static void wgr(KernelBuffer<int32> out, KernelBuffer<float32> outF,
                           KernelBuffer<float32> inF) {
        uint32 g = KernelThread.globalIdX();
        out[g] = Workgroup.reduce(GroupOp.Add, (int32) g - 300);
        outF[g] = Workgroup.reduce(GroupOp.Add, inF[g]);
    }
    @Kernel
    @Wave(width = 32)
    public static void dims(KernelBuffer<uint32> out, KernelBuffer<uint32> hits) {
        uint32 g = KernelThread.globalIdX();
        out[g * 3] = Workgroup.dimX();
        out[g * 3 + 1] = KernelThread.x();
        out[g * 3 + 2] = Wave.laneId();
        uint32 i = g;
        uint32 stride = Workgroup.dimX() * 2;
        while (i < 1000) {
            hits[i] = hits[i] + 1;
            i = i + stride;
        }
    }
    public static int32 run(boolean group) {
        uint32 n = 512;
        uint32[] hu = heap uint32[n];
        float32[] hf = heap float32[n];
        for (uint32 i = 0; i < n; i = i + 1) {
            hu[i] = (i * 7919) % 65521;
            hf[i] = (float32) i * 0.2917f - 41.3f;
        }
        KernelBuffer<uint32> inU = heap KernelBuffer<uint32>(n);
        KernelBuffer<float32> inF = heap KernelBuffer<float32>(n);
        KernelBuffer<uint32> outU = heap KernelBuffer<uint32>(n * 20);
        KernelBuffer<float32> outF = heap KernelBuffer<float32>(n * 6);
        KernelBuffer<int32> outW = heap KernelBuffer<int32>(n);
        KernelBuffer<float32> outWF = heap KernelBuffer<float32>(n);
        KernelBuffer<uint32> outD = heap KernelBuffer<uint32>(n * 3 + 1000);
        KernelBuffer<uint32> hits = heap KernelBuffer<uint32>(1000);
        inU.upload(hu);
        inF.upload(hf);
        uint32[] zeros = heap uint32[1000];
        for (uint32 i = 0; i < 1000; i = i + 1) { zeros[i] = 0; }
        hits.upload(zeros);
        KernelStream s #= KernelStream.current();
        verbs.launch(s, grid: [2], block: [256])(outU, outF, inU, inF);
        s.sync();
        wgr.launch(s, grid: [2], block: [256])(outW, outWF, inF);
        s.sync();
        dims.launch(s, grid: [2], block: [128])(outD, hits);
        s.sync();
        gotU = heap uint32[n * 20];
        gotF = heap float32[n * 6];
        gotW = heap int32[n];
        gotD = heap uint32[n * 3 + 1000];
        outU.download(gotU);
        outF.download(gotF);
        outW.download(gotW);
        uint32[] h = heap uint32[1000];
        hits.download(h);
        for (uint32 i = 0; i < 256 * 3; i = i + 1) { gotD[i] = 0; }
        uint32[] d = heap uint32[n * 3 + 1000];
        outD.download(d);
        for (uint32 i = 0; i < 256 * 3; i = i + 1) { gotD[i] = d[i]; }
        for (uint32 i = 0; i < 1000; i = i + 1) { gotD[n * 3 + i] = h[i]; }
        float32[] wf = heap float32[n];
        outWF.download(wf);
        gotG = heap float32[n * 5];
        for (uint32 i = 0; i < n; i = i + 1) { gotG[n * 4 + i] = wf[i]; }
        if (group) {
            KernelBuffer<float32> outG = heap KernelBuffer<float32>(n * 4);
            grp.launch(s, grid: [2], block: [256])(outG, inF);
            s.sync();
            float32[] gg = heap float32[n * 4];
            outG.download(gg);
            for (uint32 i = 0; i < n * 4; i = i + 1) { gotG[i] = gg[i]; }
        }
        return 1;
    }
    public static uint32 u(uint32 i) { return gotU[i]; }
    public static float32 f(uint32 i) { return gotF[i]; }
    public static float32 gr(uint32 i) { return gotG[i]; }
    public static int32 w(uint32 i) { return gotW[i]; }
    public static uint32 dd(uint32 i) { return gotD[i]; }
}
)CJ";

struct VerbOutputs {
    std::vector<uint32_t> u, d;
    std::vector<float> f, g;
    std::vector<int32_t> w;
    std::string compileText, runText;
    bool ran = false;
};

VerbOutputs runVerbsOn(cajeta::xpu::Backend be, bool group) {
    VerbOutputs o;
    CajetaJit::Options opts;
    opts.xpuBackends = {be};
    testing::internal::CaptureStderr();
    auto jit = CajetaJit::compile(kVerbSource, "test.G", opts);
    o.compileText = testing::internal::GetCapturedStderr();
    EXPECT_NE(jit, nullptr) << o.compileText;
    if (!jit) return o;
    auto fn = jit->lookup<int32_t (*)(bool)>("run");
    auto uAt = jit->lookup<uint32_t (*)(uint32_t)>("u");
    auto fAt = jit->lookup<float (*)(uint32_t)>("f");
    auto gAt = jit->lookup<float (*)(uint32_t)>("gr");
    auto wAt = jit->lookup<int32_t (*)(uint32_t)>("w");
    auto dAt = jit->lookup<uint32_t (*)(uint32_t)>("dd");
    EXPECT_TRUE(fn && uAt && fAt && gAt && wAt && dAt);
    if (!(fn && uAt && fAt && gAt && wAt && dAt)) return o;
    testing::internal::CaptureStderr();
    int32_t r = fn(group);
    o.runText = testing::internal::GetCapturedStderr();
    EXPECT_EQ(r, 1) << o.runText;
    for (uint32_t k = 0; k < kVN * 20; ++k) o.u.push_back(uAt(k));
    for (uint32_t k = 0; k < kVN * 6; ++k) o.f.push_back(fAt(k));
    for (uint32_t k = 0; k < kVN * 5; ++k) o.g.push_back(gAt(k));
    for (uint32_t k = 0; k < kVN; ++k) o.w.push_back(wAt(k));
    for (uint32_t k = 0; k < kVN * 3 + 1000; ++k) o.d.push_back(dAt(k));
    o.ran = true;
    return o;
}

}  // namespace

// 2.1.1 to 2.1.4: every wave verb, Group, Workgroup.reduce and the logical ids run virtually
// at W = 32 and match the cpu bit for bit, the Group verbs matching their Wave equivalents.
TEST(XpuVulkanVirtualWave, everyWaveVerbRunsVirtuallyAndMatchesTheCpu) {
    if (cajeta::xpu::vulkan::VulkanDriver::canRunSubgroupWidth(32))
        GTEST_SKIP() << "this Vulkan device pins 32, so nothing runs virtually";
    VerbOutputs dev = runVerbsOn(cajeta::xpu::Backend::Spirv, true);
    ASSERT_TRUE(dev.ran);
    EXPECT_EQ(dev.compileText.find("skipped]"), std::string::npos) << dev.compileText;
    EXPECT_EQ(dev.runText.find("xpu-launch-refused"), std::string::npos) << dev.runText;
    VerbOutputs cpu = runVerbsOn(cajeta::xpu::Backend::Cpu, false);
    ASSERT_TRUE(cpu.ran);
    const char* uNames[] = {"shuffle", "shuffleUp", "shuffleDown", "rotate", "ballot.lo",
                            "ballot.hi", "reduceSum", "reduceMax", "reduceMin", "reduceAnd",
                            "reduceOr", "reduceXor", "prefixSum", "prefixProduct",
                            "Quad.broadcast", "Quad.swapDiagonal", "Quad.any/all",
                            "divergent reduce+scan", "divergent ballot", "width"};
    for (size_t k = 0; k < cpu.u.size(); ++k) {
        ASSERT_EQ(dev.u[k], cpu.u[k]) << uNames[k % 20] << " at work-item " << k / 20;
    }
    const char* fNames[] = {"reduceSumF32", "reduceMaxF32", "segmented sum 8",
                            "segmented max 4", "shuffleDownF32", "divergent reduceSumF32"};
    for (size_t k = 0; k < cpu.f.size(); ++k)
        ASSERT_EQ(bits(dev.f[k]), bits(cpu.f[k])) << fNames[k % 6] << " at work-item " << k / 6
            << ": " << dev.f[k] << " vs cpu " << cpu.f[k];
    for (size_t k = 0; k < cpu.w.size(); ++k)
        ASSERT_EQ(dev.w[k], cpu.w[k]) << "Workgroup.reduce int at " << k;
    for (size_t k = 0; k < kVN; ++k)
        ASSERT_EQ(bits(dev.g[kVN * 4 + k]), bits(cpu.g[kVN * 4 + k]))
            << "Workgroup.reduce float at " << k;
    for (size_t k = 0; k < cpu.d.size(); ++k)
        ASSERT_EQ(dev.d[k], cpu.d[k]) << (k < kVN * 3 ? "ids and dims" : "grid-stride hits")
            << " at " << k;
    for (uint32_t i = 0; i < kVN; ++i) {
        ASSERT_EQ(bits(dev.g[i * 4]), bits(cpu.f[i * 6])) << "Group.reduce Add at " << i;
        ASSERT_EQ(bits(dev.g[i * 4 + 1]), bits(cpu.f[i * 6 + 1])) << "Group.reduce Max at " << i;
        ASSERT_EQ(dev.g[i * 4 + 2], (float) (i % 32)) << "Group.laneId";
        ASSERT_EQ(dev.g[i * 4 + 3], 32.0f) << "Group.width";
    }
}

namespace {

const char* kRefusalSource = R"CJ(
package test;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelThread;
import cajeta.xpu.Wave;
public class R {
    @Kernel
    @Wave(width = 32)
    public static void laneLoop(KernelBuffer<uint32> out, KernelBuffer<uint32> in) {
        uint32 g = KernelThread.globalIdX();
        uint32 lane = Wave.laneId();
        uint32 acc = 0;
        uint32 j = 0;
        while (j < lane) {
            acc = acc + Wave.shuffleSync(in[g], j);
            j = j + 1;
        }
        out[g] = acc;
    }
    @Kernel
    @Wave(width = 32)
    public static void uniformLoop(KernelBuffer<uint32> out, KernelBuffer<uint32> in) {
        uint32 g = KernelThread.globalIdX();
        uint32 acc = 0;
        uint32 j = 0;
        while (j < 5) {
            acc = acc + Wave.shuffleSync(in[g], j);
            j = j + 1;
        }
        out[g] = acc;
    }
}
)CJ";

}  // namespace

// 2.1.5: a wave verb in a loop whose trip count differs between lanes has no virtual variant,
// refused by name with the native kernel kept; its twin in a uniform loop lowers.
TEST(XpuVulkanVirtualWave, aVerbInALaneCountedLoopIsRefusedAndItsUniformTwinLowers) {
    CajetaJit::Options opts;
    opts.xpuBackends = {cajeta::xpu::Backend::Spirv};
    testing::internal::CaptureStderr();
    auto jit = CajetaJit::compile(kRefusalSource, "test.R", opts);
    std::string text = testing::internal::GetCapturedStderr();
    ASSERT_NE(jit, nullptr) << text;
    EXPECT_NE(text.find("[xpu-virtual-skipped] laneLoop$v8"), std::string::npos) << text;
    EXPECT_NE(text.find("in a virtual wave of 32 lanes over subgroups of 8"), std::string::npos) << text;
    EXPECT_EQ(text.find("[xpu-kernel-skipped]"), std::string::npos) << text;
    EXPECT_EQ(text.find("uniformLoop"), std::string::npos) << text;
}
