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
// narrow: the same access with no stated alignment; bytes, halves: 16-byte groups of 8- and 16-bit lanes;
// bytemix: a byte group beside a single-byte read of the same buffer; bytewords: the same stated 4-byte aligned;
// viaHelper: a half group read in a helper the buffer is passed to; bytes32: 32-byte groups at both alignments.
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
    @Kernel
    public static void bytes(KernelBuffer<int32> out, KernelBuffer<int8> y, KernelBuffer<int8> x) {
        uint32 t = KernelThread.globalIdX();
        uint32 base = t * 16;
        Vector<int8,16> v = x.vload<16>(base, align: 16);
        y.vstore(base, v, align: 16);
        out[t] = (int32) v[0] + (int32) v[15] * 256;
    }
    @Kernel
    public static void halves(KernelBuffer<float32> out, KernelBuffer<float16> x) {
        uint32 t = KernelThread.globalIdX();
        Vector<float16,8> v = x.vload<8>(t * 8, align: 16);
        out[t] = (float32) v[0] + (float32) v[7] * 1000.0f;
    }
    @Kernel
    public static void bytemix(KernelBuffer<int32> out, KernelBuffer<int8> x) {
        uint32 t = KernelThread.globalIdX();
        Vector<int8,16> v = x.vload<16>(t * 16, align: 16);
        out[t] = (int32) v[1] + (int32) x[t * 16 + 2] * 256;
    }
    @Kernel
    public static void bytewords(KernelBuffer<int32> out, KernelBuffer<int8> x) {
        uint32 t = KernelThread.globalIdX();
        Vector<int8,16> v = x.vload<16>(t * 20 + 4, align: 4);
        out[t] = (int32) v[0] + (int32) v[15] * 256 + (int32) x[t * 20 + 1] * 65536;
    }
    @Device
    static float32 halfPair(KernelBuffer<float16> x, uint32 at) {
        Vector<float16,8> v = x.vload<8>(at, align: 16);
        return (float32) v[0] + (float32) v[7] * 1000.0f;
    }
    @Kernel
    public static void viaHelper(KernelBuffer<float32> out, KernelBuffer<float16> x) {
        uint32 t = KernelThread.globalIdX();
        out[t] = W.halfPair(x, t * 8);
    }
    @Kernel
    public static void bytes32(KernelBuffer<int32> out, KernelBuffer<int8> x) {
        uint32 t = KernelThread.globalIdX();
        Vector<int8,32> a = x.vload<32>(t * 64, align: 16);
        Vector<int8,32> b = x.vload<32>(t * 64 + 28, align: 4);
        out[t] = (int32) a[0] + (int32) a[31] * 256 + (int32) b[0] * 65536 + (int32) b[31] * 16777216;
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
        int8[] h8 = heap int8[n];
        int8[] g8 = heap int8[n];
        int32[] o8 = heap int32[n / 16];
        int32[] m8 = heap int32[n / 16];
        float16[] h16 = heap float16[n];
        float32[] o16 = heap float32[n / 8];
        for (uint32 i = 0; i < n; i = i + 1) { h8[i] = (int8) (i % 100); h16[i] = (float16) (float32) (i % 64); }
        KernelBuffer<int8> x8 = heap KernelBuffer<int8>(n);
        KernelBuffer<int8> y8 = heap KernelBuffer<int8>(n);
        KernelBuffer<int32> out8 = heap KernelBuffer<int32>(n / 16);
        KernelBuffer<int32> mix8 = heap KernelBuffer<int32>(n / 16);
        KernelBuffer<float16> x16 = heap KernelBuffer<float16>(n);
        KernelBuffer<float32> out16 = heap KernelBuffer<float32>(n / 8);
        x8.upload(h8);
        x16.upload(h16);
        bytes.launch(s, grid: [1], block: [16])(out8, y8, x8);
        bytemix.launch(s, grid: [1], block: [16])(mix8, x8);
        halves.launch(s, grid: [1], block: [32])(out16, x16);
        KernelBuffer<float32> viaOut = heap KernelBuffer<float32>(n / 8);
        float32[] hvia = heap float32[n / 8];
        viaHelper.launch(s, grid: [1], block: [32])(viaOut, x16);
        KernelBuffer<int32> o32 = heap KernelBuffer<int32>(3);
        int32[] h32 = heap int32[3];
        bytes32.launch(s, grid: [1], block: [3])(o32, x8);
        KernelBuffer<int32> w8 = heap KernelBuffer<int32>(12);
        int32[] hw8 = heap int32[12];
        bytewords.launch(s, grid: [1], block: [12])(w8, x8);
        s.sync();
        y8.download(g8);
        out8.download(o8);
        mix8.download(m8);
        out16.download(o16);
        w8.download(hw8);
        viaOut.download(hvia);
        o32.download(h32);
        for (uint32 t = 0; t < 3; t = t + 1) {
            int32 want = (int32) h8[t * 64] + (int32) h8[t * 64 + 31] * 256 + (int32) h8[t * 64 + 28] * 65536
                + (int32) h8[t * 64 + 59] * 16777216;
            if (h32[t] != want) { return (int32) (1200 + t); }
        }
        for (uint32 t = 0; t < n / 8; t = t + 1) {
            if (hvia[t] != o16[t]) { return (int32) (1100 + t); }
        }
        for (uint32 t = 0; t < 12; t = t + 1) {
            int32 want = (int32) h8[t * 20 + 4] + (int32) h8[t * 20 + 19] * 256 + (int32) h8[t * 20 + 1] * 65536;
            if (hw8[t] != want) { return (int32) (1000 + t); }
        }
        for (uint32 i = 0; i < n; i = i + 1) {
            if (g8[i] != h8[i]) { return (int32) (400 + i); }
        }
        for (uint32 t = 0; t < n / 16; t = t + 1) {
            if (o8[t] != (int32) h8[t * 16] + (int32) h8[t * 16 + 15] * 256) { return (int32) (700 + t); }
            if (m8[t] != (int32) h8[t * 16 + 1] + (int32) h8[t * 16 + 2] * 256) { return (int32) (800 + t); }
        }
        for (uint32 t = 0; t < n / 8; t = t + 1) {
            if (o16[t] != (float32) ((t * 8) % 64) + (float32) ((t * 8 + 7) % 64) * 1000.0f) { return (int32) (900 + t); }
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

// A 16-lane byte group and an 8-lane half group each load and store as one 16-byte word vector.
TEST(SpirvWideAccessTests, anAlignedByteGroupIsOneSixteenByteLoadAndStore) {
    std::string failure;
    auto spirv = lower("bytes", failure);
    ASSERT_FALSE(spirv.empty()) << failure;
    auto valid = spirvVal(spirv);
    auto dis = spirvDis(spirv);
    if (!valid || !dis) GTEST_SKIP() << "spirv-val or spirv-dis not installed";
    EXPECT_TRUE(*valid) << *dis;
    EXPECT_EQ(count(*dis, "OpLoad %v4uint"), 1) << *dis;
    EXPECT_GE(count(*dis, "OpAccessChain %_ptr_StorageBuffer_v4uint"), 2) << *dis;
}

TEST(SpirvWideAccessTests, anAlignedHalfGroupIsOneSixteenByteLoad) {
    std::string failure;
    auto spirv = lower("halves", failure);
    ASSERT_FALSE(spirv.empty()) << failure;
    auto valid = spirvVal(spirv);
    auto dis = spirvDis(spirv);
    if (!valid || !dis) GTEST_SKIP() << "spirv-val or spirv-dis not installed";
    EXPECT_TRUE(*valid) << *dis;
    EXPECT_EQ(count(*dis, "OpLoad %v4uint"), 1) << *dis;
    EXPECT_GE(count(*dis, "OpAccessChain %_ptr_StorageBuffer_v4uint"), 1) << *dis;
}

// A byte buffer read both as a group and as one byte keeps its byte type and validates.
TEST(SpirvWideAccessTests, aByteBufferReadAlsoAsOneByteValidates) {
    std::string failure;
    auto spirv = lower("bytemix", failure);
    ASSERT_FALSE(spirv.empty()) << failure;
    auto valid = spirvVal(spirv);
    auto dis = spirvDis(spirv);
    if (!valid || !dis) GTEST_SKIP() << "spirv-val or spirv-dis not installed";
    EXPECT_TRUE(*valid) << *dis;
}

// A group read in a helper widens once the helper is inlined into the kernel that binds the buffer.
TEST(SpirvWideAccessTests, aGroupReadInAHelperIsOneSixteenByteLoad) {
    std::string failure;
    auto spirv = lower("viaHelper", failure);
    ASSERT_FALSE(spirv.empty()) << failure;
    auto valid = spirvVal(spirv);
    auto dis = spirvDis(spirv);
    if (!valid || !dis) GTEST_SKIP() << "spirv-val or spirv-dis not installed";
    EXPECT_TRUE(*valid) << *dis;
    EXPECT_EQ(count(*dis, "OpLoad %v4uint"), 1) << *dis;
    EXPECT_EQ(count(*dis, "OpAccessChain %_ptr_StorageBuffer_half"), 0) << *dis;
}

// A 32-byte group reads as two 16-byte units stated 16-byte aligned and as eight words stated 4-byte aligned.
TEST(SpirvWideAccessTests, aThirtyTwoByteGroupReadsWideUnits) {
    std::string failure;
    auto spirv = lower("bytes32", failure);
    ASSERT_FALSE(spirv.empty()) << failure;
    auto valid = spirvVal(spirv);
    auto dis = spirvDis(spirv);
    if (!valid || !dis) GTEST_SKIP() << "spirv-val or spirv-dis not installed";
    EXPECT_TRUE(*valid) << *dis;
    EXPECT_EQ(count(*dis, "OpAccessChain %_ptr_StorageBuffer_uchar"), 0) << *dis;
}

// A byte group stated 4-byte aligned reads whole words, and the buffer's single-byte read moves onto them.
TEST(SpirvWideAccessTests, aWordAlignedByteGroupReadsWholeWords) {
    std::string failure;
    auto spirv = lower("bytewords", failure);
    ASSERT_FALSE(spirv.empty()) << failure;
    auto valid = spirvVal(spirv);
    auto dis = spirvDis(spirv);
    if (!valid || !dis) GTEST_SKIP() << "spirv-val or spirv-dis not installed";
    EXPECT_TRUE(*valid) << *dis;
    EXPECT_EQ(count(*dis, "OpAccessChain %_ptr_StorageBuffer_uchar"), 0) << *dis;
    EXPECT_GE(count(*dis, "OpLoad %uint"), 5) << *dis;
}

TEST(SpirvWideAccessTests, wideAndScalarAccessMatchTheHostOnVulkan) {
    if (!cajeta::xpu::test::vulkanAvailable()) GTEST_SKIP() << "no Vulkan device";
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Spirv};
    auto jit = CajetaJit::compile(kSource, "test.W", o);
    ASSERT_NE(jit, nullptr);
    auto run = jit->lookup<int (*)()>("run");
    ASSERT_NE(run, nullptr);
    EXPECT_EQ(run(), 777) << "100+t: a wide store; 200+t: a scalar read after it; 300+i: float lanes; "
                             "400+i: byte copy; 700+t, 800+t: byte lanes; 900+t: half lanes; 1000+t: word-aligned bytes; 1100+t: helper; 1200+t: 32-byte groups";
}
