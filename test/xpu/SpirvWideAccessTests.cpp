//
// SpirvWideAccessTests — a 32-bit vload<4>/vstore stated 16-byte aligned reaches Vulkan as one 16-byte
// access through a vector view of the buffer's binding, beside scalar access to the same buffer
// (gpu-kernel-parity 16.2.4.3).
//
#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "KernelLoweringProbe.h"
#include "XpuDeviceTestUtil.h"
#include "cajeta/xpu/XpuTarget.h"
#include "cajeta/xpu/vulkan/SpirvBackend.h"
#include "cajeta/xpu/vulkan/SpirvKernelLowering.h"
#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/method/Method.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

using cajeta_test::CajetaJit;
using cajeta::xpu::probe::compileForInspection;
using cajeta::xpu::probe::findMethod;

namespace {

// atom: an aligned group on a buffer that also takes an atomic;
// mix: a scalar store, a wide store over it and a scalar read back of the same words; flt: float lanes;
// narrow: the same access with no stated alignment.
const char* kSource = R"CJ(
package test;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
import cajeta.xpu.KernelThread;
import cajeta.xpu.Vector;
import cajeta.xpu.MemoryOrder;
public class W {
    @Kernel
    public static void mix(KernelBuffer<int32> out, KernelBuffer<int32> back, KernelBuffer<int32> x) {
        uint32 t = KernelThread.globalIdX();
        uint32 base = t * 4;
        Vector<int32,4> q = x.vload<4>(base, align: 16);
        int32 s = x[base + 1];
        out[base] = 1000;
        out.vstore(base, heap Vector<int32,4>(q.x * 2 + s, q.y * 2 + s, q.z * 2 + s, q.w * 2 + s), align: 16);
        back[t] = out[base] + out[base + 3];
    }
    @Kernel
    public static void flt(KernelBuffer<float32> y, KernelBuffer<float32> a) {
        uint32 base = KernelThread.globalIdX() * 4;
        Vector<float32,4> v = a.vload<4>(base, align: 16);
        y.vstore(base, heap Vector<float32,4>(v.x * 0.5f, v.y * 0.5f, v.z * 0.5f, v.w * 0.5f), align: 16);
    }
    @Kernel
    public static void atom(KernelBuffer<int32> out, KernelBuffer<int32> x) {
        uint32 base = KernelThread.globalIdX() * 4;
        Vector<int32,4> q = x.vload<4>(base, align: 16);
        x.atomicAdd(base, 1, MemoryOrder.Relaxed);
        out[base] = q.x + q.w;
    }
    @Kernel
    public static void narrow(KernelBuffer<int32> out, KernelBuffer<int32> x) {
        uint32 base = KernelThread.globalIdX() * 4;
        Vector<int32,4> q = x.vload<4>(base + 1);
        out[base] = q.x + q.w;
    }
    public static int32 run() {
        uint32 n = 256;
        int32[] hx = heap int32[n];
        int32[] ho = heap int32[n];
        int32[] hb = heap int32[n / 4];
        float32[] ha = heap float32[n];
        float32[] hy = heap float32[n];
        for (uint32 i = 0; i < n; i = i + 1) { hx[i] = (int32) (i * 3); ha[i] = (float32) i; }
        KernelBuffer<int32> x = heap KernelBuffer<int32>(n);
        KernelBuffer<int32> out = heap KernelBuffer<int32>(n);
        KernelBuffer<int32> back = heap KernelBuffer<int32>(n / 4);
        KernelBuffer<float32> a = heap KernelBuffer<float32>(n);
        KernelBuffer<float32> y = heap KernelBuffer<float32>(n);
        x.upload(hx);
        a.upload(ha);
        KernelStream s #= KernelStream.current();
        mix.launch(s, grid: [1], block: [64])(out, back, x);
        flt.launch(s, grid: [1], block: [64])(y, a);
        s.sync();
        out.download(ho);
        back.download(hb);
        y.download(hy);
        for (uint32 t = 0; t < n / 4; t = t + 1) {
            int32 sc = hx[t * 4 + 1];
            for (uint32 j = 0; j < 4; j = j + 1) {
                if (ho[t * 4 + j] != hx[t * 4 + j] * 2 + sc) { return (int32) (100 + t); }
            }
            if (hb[t] != ho[t * 4] + ho[t * 4 + 3]) { return (int32) (200 + t); }
        }
        for (uint32 i = 0; i < n; i = i + 1) {
            if (hy[i] != (float32) i * 0.5f) { return (int32) (300 + i); }
        }
        return 777;
    }
}
)CJ";

std::optional<bool> spirvVal(const std::vector<uint8_t>& spirv) {
    auto tool = llvm::sys::findProgramByName("spirv-val");
    if (!tool) return std::nullopt;
    llvm::SmallString<128> path;
    if (llvm::sys::fs::createTemporaryFile("cajeta-wide", "spv", path)) return std::nullopt;
    { std::ofstream f(path.c_str(), std::ios::binary);
      f.write(reinterpret_cast<const char*>(spirv.data()), (std::streamsize) spirv.size()); }
    std::string p = path.str().str();
    llvm::StringRef args[] = {*tool, "--target-env", "vulkan1.3", p};
    int rc = llvm::sys::ExecuteAndWait(*tool, args);
    llvm::sys::fs::remove(path);
    return rc == 0;
}

// The SPIR-V disassembly, or nullopt without spirv-dis.
std::optional<std::string> spirvDis(const std::vector<uint8_t>& spirv) {
    auto tool = llvm::sys::findProgramByName("spirv-dis");
    if (!tool) return std::nullopt;
    llvm::SmallString<128> in, out;
    if (llvm::sys::fs::createTemporaryFile("cajeta-wide", "spv", in)
            || llvm::sys::fs::createTemporaryFile("cajeta-wide", "txt", out))
        return std::nullopt;
    { std::ofstream f(in.c_str(), std::ios::binary);
      f.write(reinterpret_cast<const char*>(spirv.data()), (std::streamsize) spirv.size()); }
    std::string ip = in.str().str(), op = out.str().str();
    llvm::StringRef args[] = {*tool, ip, "-o", op};
    int rc = llvm::sys::ExecuteAndWait(*tool, args);
    std::ifstream t(op);
    std::string text((std::istreambuf_iterator<char>(t)), std::istreambuf_iterator<char>());
    llvm::sys::fs::remove(in);
    llvm::sys::fs::remove(out);
    if (rc != 0) return std::nullopt;
    return text;
}

int count(const std::string& hay, const std::string& needle) {
    int n = 0;
    for (size_t at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + 1)) ++n;
    return n;
}

// Kernel `name`'s SPIR-V.
std::vector<uint8_t> lower(const char* name, std::string& failure) {
    using namespace cajeta::xpu::vulkan;
    cajeta::Compiler compiler;
    auto module = compileForInspection(compiler, kSource, "test.W", std::string("spvwide_") + name);
    auto k = findMethod(module->getStructures()["test.W"], name);
    if (!k) { failure = "no kernel"; return {}; }
    auto tm = createSpirvTargetMachine("vulkan1.3");
    llvm::LLVMContext ctx;
    llvm::Module dev("xpu_wide", ctx);
    configureDeviceModule(dev, *tm);
    if (!lowerKernel(k, dev)) { failure = "lowering failed"; return {}; }
    return emitSpirv(dev, *tm, &failure);
}

} // namespace

// A buffer read and written both in 16-byte groups and as single words takes the vector type, and
// the module validates.
TEST(SpirvWideAccessTests, anAlignedIntGroupIsOneSixteenByteLoadAndStore) {
    std::string failure;
    auto spirv = lower("mix", failure);
    ASSERT_FALSE(spirv.empty()) << failure;
    auto valid = spirvVal(spirv);
    auto dis = spirvDis(spirv);
    if (!valid || !dis) GTEST_SKIP() << "spirv-val or spirv-dis not installed";
    EXPECT_TRUE(*valid) << "spirv-val rejected a buffer read both wide and scalar\n" << *dis;
    EXPECT_GE(count(*dis, "OpLoad %v4uint"), 1) << *dis;
    EXPECT_GE(count(*dis, "OpAccessChain %_ptr_StorageBuffer_v4uint"), 2) << *dis;
}

TEST(SpirvWideAccessTests, anAlignedFloatGroupIsOneSixteenByteLoadAndStore) {
    std::string failure;
    auto spirv = lower("flt", failure);
    ASSERT_FALSE(spirv.empty()) << failure;
    auto valid = spirvVal(spirv);
    auto dis = spirvDis(spirv);
    if (!valid || !dis) GTEST_SKIP() << "spirv-val or spirv-dis not installed";
    EXPECT_TRUE(*valid) << *dis;
    EXPECT_GE(count(*dis, "OpLoad %v4float"), 1) << *dis;
    EXPECT_GE(count(*dis, "OpAccessChain %_ptr_StorageBuffer_v4float"), 2) << *dis;
}

// A buffer that also takes an atomic keeps its word type, and its 16-byte group splits back into words.
TEST(SpirvWideAccessTests, aBufferWithAnAtomicStaysWordTyped) {
    std::string failure;
    auto spirv = lower("atom", failure);
    ASSERT_FALSE(spirv.empty()) << failure;
    auto valid = spirvVal(spirv);
    auto dis = spirvDis(spirv);
    if (!valid || !dis) GTEST_SKIP() << "spirv-val or spirv-dis not installed";
    EXPECT_TRUE(*valid) << *dis;
    EXPECT_EQ(count(*dis, "StorageBuffer_v4uint") + count(*dis, "runtimearr_v4uint"), 0) << *dis;
}

TEST(SpirvWideAccessTests, anUnstatedGroupStaysScalar) {
    std::string failure;
    auto spirv = lower("narrow", failure);
    ASSERT_FALSE(spirv.empty()) << failure;
    auto dis = spirvDis(spirv);
    if (!dis) GTEST_SKIP() << "spirv-dis not installed";
    EXPECT_EQ(count(*dis, "StorageBuffer_v4uint") + count(*dis, "runtimearr_v4uint"), 0) << *dis;
}

TEST(SpirvWideAccessTests, wideAndScalarAccessMatchTheHostOnVulkan) {
    if (!cajeta::xpu::test::vulkanAvailable()) GTEST_SKIP() << "no Vulkan device";
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Spirv};
    auto jit = CajetaJit::compile(kSource, "test.W", o);
    ASSERT_NE(jit, nullptr);
    auto run = jit->lookup<int (*)()>("run");
    ASSERT_NE(run, nullptr);
    EXPECT_EQ(run(), 777) << "100+t: a wide store; 200+t: a scalar read after it; 300+i: float lanes";
}
