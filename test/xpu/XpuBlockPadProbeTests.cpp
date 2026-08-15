//
// CajetaXPU — BlockPadded<T,Block,Pad> LDS layout probe (gpu-f16-torch-parity U1). GPU-free.
//
// torch's verified gfx1151 f16 GEMM uses Tensile's LdsBlockSizePerPad/LdsPad: physical =
// logical + (logical/Block)*Pad, a fixed byte-period pad independent of row/col, applied
// identically to the wide staging store AND the WMMA fragment read. Unlike the per-row XOR
// Swizzled<T,S> (which scalarizes wide reads), block padding shifts whole 128-bit chunks, so
// ds_store_b128 / ds_read_b128 survive. This probe asserts (1) a BlockPadded tile staged with a
// wide store keeps ds_store_b128 and its col-major fragment read keeps ds_read_b128 (no
// ds_read_u16, no spill, clean lowering); (2) the additive-pad map is applied consistently at
// direct access (write-map == read-map, in bounds) via a single-thread CPU JIT round-trip across
// multiple blocks; (3) the pad is actually applied (differs from a plain Shared tile).
//

#include "gtest/gtest.h"

#include "cajeta/xpu/amd/AmdgpuBackend.h"
#include "cajeta/xpu/amd/AmdgpuKernelLowering.h"
#include "cajeta/xpu/cpu/CpuKernelLowering.h"
#include "cajeta/xpu/cpu/CpuBackend.h"

#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/method/Method.h"

#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "jit/CoffSafeJit.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <vector>

using namespace cajeta::xpu::amd;
using cajeta::Compiler;
using cajeta::CajetaModulePtr;

namespace {

// B tile [N=128][K=64] stored CONTIGUOUSLY (wide vstore, torch GRVWB8) into a BlockPadded tile,
// read with the transposing col-major fragment (use=1). Block=128 elems (256B for f16), Pad=16.
// Padded length = 128*64 + (128*64/128)*16 = 8192 + 1024 = 9216.
const char* kBlockPadWidthSrc =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "import cajeta.xpu.Barrier;\n"
    "import cajeta.xpu.BlockPadded;\n"
    "import cajeta.xpu.CooperativeMatrix;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void bpw(KernelBuffer<float32> c, KernelBuffer<float16> b, uint32 n) {\n"
    "        BlockPadded<float16,128,16> sb = shared float16[9216];\n"
    "        uint32 e = KernelThread.x();\n"
    "        while (e < 1024) {\n"
    "            Vector<float16,8> v = b.vload<8>(e * 8);\n"
    "            sb.vstore(e * 8, v);\n"            // contiguous wide store, compiler pads
    "            e = e + 128;\n"
    "        }\n"
    "        Barrier.workgroup();\n"
    "        CooperativeMatrix<float16,16,16,1> wb; wb.load(sb, 0, 1, 64);\n"  // logical stride 64
    "        CooperativeMatrix<float16,16,16,0> wa; wa.load(sb, 0, 0, 64);\n"
    "        CooperativeMatrix<float32,16,16,2> acc; acc.splat(0.0f);\n"
    "        acc.mma(wa, wb);\n"
    "        acc.store(c, 0, 0, n);\n"
    "    }\n"
    "}\n";

CajetaModulePtr compileForInspection(Compiler& compiler, const char* source) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_xpu_blockpad_" + std::to_string(rng()));
    std::filesystem::create_directories(base / "test");
    std::ofstream(base / "test" / "M.cajeta") << source;
    auto archive = std::filesystem::temp_directory_path()
                 / ("cajeta_xpu_blockpad_arch_" + std::to_string(rng()));
    std::filesystem::create_directories(archive);
    auto m = compiler.createModule((base / "test" / "M.cajeta").string(),
                                   base.string(), archive.string());
    compiler.compile(m);
    return m;
}

cajeta::MethodPtr findMethod(const cajeta::CajetaClassPtr& klass,
                             const std::string& name) {
    for (auto& [k, m] : klass->getMethods())
        if (m && m->getName() == name) return m;
    return nullptr;
}

int parseAmdMeta(const std::string& isa, const std::string& key) {
    std::istringstream ss(isa);
    std::string line;
    std::string needle = "." + key + ":";
    while (std::getline(ss, line)) {
        auto p = line.find(needle);
        if (p == std::string::npos) continue;
        p += needle.size();
        while (p < line.size() && !std::isdigit((unsigned char) line[p])) ++p;
        if (p >= line.size()) return -1;
        return std::atoi(line.c_str() + p);
    }
    return -1;
}

std::string isaOf(const char* source, const char* kernelName) {
    Compiler compiler;
    auto module = compileForInspection(compiler, source);
    auto method = findMethod(module->getStructures()["test.M"], kernelName);
    if (!method) return {};
    auto tm = createAmdgpuTargetMachine("gfx1151");
    if (!tm) return {};
    llvm::LLVMContext ctx;
    llvm::Module dev("xpu_blockpad_isa", ctx);
    configureDeviceModule(dev, *tm);
    if (!lowerKernel(method, dev)) return {};
    return cajeta::xpu::amd::emitIsa(dev, *tm);
}

int countOccurrences(const std::string& hay, const std::string& needle) {
    int n = 0;
    for (size_t p = hay.find(needle); p != std::string::npos;
         p = hay.find(needle, p + needle.size()))
        ++n;
    return n;
}

} // namespace

// 1.1.1 + 1.1.2 — block padding keeps the wide staging store AND the wide fragment read wide.
TEST(XpuBlockPadProbeTests, blockPaddedKeepsWideStoreAndRead) {
    std::string isa = isaOf(kBlockPadWidthSrc, "bpw");
    ASSERT_FALSE(isa.empty()) << "block-padded kernel failed to lower";
    EXPECT_EQ(isa.find("Cannot select"), std::string::npos) << isa;
    int spill = parseAmdMeta(isa, "vgpr_spill_count");
    int dsW128 = countOccurrences(isa, "ds_write_b128") + countOccurrences(isa, "ds_store_b128");
    int dsR128 = countOccurrences(isa, "ds_read_b128") + countOccurrences(isa, "ds_load_b128");
    int dsRu16 = countOccurrences(isa, "ds_read_u16") + countOccurrences(isa, "ds_load_u16");
    std::cerr << "[blockpad] BlockPadded<f16,128,16> wide store + col-major read:\n"
              << "  vgpr_spill=" << spill << "  ds_store_b128=" << dsW128
              << "  ds_read_b128=" << dsR128 << "  ds_read_u16=" << dsRu16 << "\n";
    EXPECT_EQ(spill, 0) << "block-padded kernel should not spill";
    EXPECT_GT(dsW128, 0) << "wide contiguous store must stay ds_store_b128 under block padding";
    EXPECT_GT(dsR128, 0) << "col-major fragment read must stay ds_read_b128 under block padding";
    EXPECT_EQ(dsRu16, 0) << "block padding must not scalarize the fragment read (ds_read_u16)";
}

// 1.1.3 — the additive-pad map is applied CONSISTENTLY at direct access (write-map == read-map)
// and stays in bounds, across multiple blocks, via a single-thread CPU JIT round-trip.
// BlockPadded<float32,16,4>: 64 logical elems span 4 blocks; padded length 64 + (64/16)*4 = 80.
namespace {
const char* kBlockPadRoundtripSrc =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "import cajeta.xpu.Workgroup;\n"
    "import cajeta.xpu.BlockPadded;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void rt(KernelBuffer<float32> out, KernelBuffer<float32> b, uint32 n) {\n"
    "        BlockPadded<float32,16,4> t = shared float32[80];\n"
    "        uint32 e = KernelThread.x();\n"
    "        while (e < 64) { t[e] = b[e]; e = e + Workgroup.dimX(); }\n"
    "        uint32 f = KernelThread.x();\n"
    "        while (f < 64) { out[f] = t[f]; f = f + Workgroup.dimX(); }\n"
    "    }\n"
    "}\n";
using RtFn = void (*)(float*, float*, uint32_t,
                      int32_t, int32_t, int32_t, int32_t, int32_t, int32_t,
                      int32_t, int32_t, int32_t, int32_t, int32_t, int32_t);
} // namespace

TEST(XpuBlockPadProbeTests, blockPadMapIsConsistentOnCpu) {
    Compiler compiler;
    auto module = compileForInspection(compiler, kBlockPadRoundtripSrc);
    auto k = findMethod(module->getStructures()["test.M"], "rt");
    ASSERT_NE(k, nullptr);
    auto tm = cajeta::xpu::cpu::createCpuTargetMachine();
    ASSERT_NE(tm, nullptr);
    auto ctx = std::make_unique<llvm::LLVMContext>();
    auto host = std::make_unique<llvm::Module>("xpu_blockpad_cpu", *ctx);
    cajeta::xpu::cpu::configureHostModule(*host, *tm);
    auto* fn = cajeta::xpu::cpu::lowerKernel(k, *host);
    ASSERT_NE(fn, nullptr);
    std::string fnName = fn->getName().str();
    auto jitOrErr = cajeta::test::makeCoffSafeJit();
    ASSERT_TRUE(static_cast<bool>(jitOrErr)) << llvm::toString(jitOrErr.takeError());
    auto jit = std::move(*jitOrErr);
    auto err = jit->addIRModule(
        llvm::orc::ThreadSafeModule(std::move(host), std::move(ctx)));
    ASSERT_FALSE(static_cast<bool>(err)) << llvm::toString(std::move(err));
    auto symOrErr = jit->lookup(fnName);
    ASSERT_TRUE(static_cast<bool>(symOrErr)) << llvm::toString(symOrErr.takeError());
    auto rt = symOrErr->toPtr<RtFn>();

    std::vector<float> b(64), out(64, -1.0f);
    for (int i = 0; i < 64; ++i) b[i] = static_cast<float>(i * 3 + 1);
    rt(out.data(), b.data(), 64, 0,0,0, 0,0,0, 1,1,1, 1,1,1);
    for (int i = 0; i < 64; ++i)
        EXPECT_FLOAT_EQ(out[i], b[i]) << "block-pad map inconsistent at logical i=" << i;
}

// 1.1.4 (part) — the pad is ACTUALLY applied: a BlockPadded tile's addressing differs from a
// plain Shared tile of the same logical access (proves it is not silently the identity).
namespace {
const char* kPlainSharedSrc =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "import cajeta.xpu.Barrier;\n"
    "import cajeta.xpu.Shared;\n"
    "import cajeta.xpu.CooperativeMatrix;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void bpw(KernelBuffer<float32> c, KernelBuffer<float16> b, uint32 n) {\n"
    "        Shared<float16> sb = shared float16[9216];\n"
    "        uint32 e = KernelThread.x();\n"
    "        while (e < 1024) {\n"
    "            Vector<float16,8> v = b.vload<8>(e * 8);\n"
    "            sb.vstore(e * 8, v);\n"
    "            e = e + 128;\n"
    "        }\n"
    "        Barrier.workgroup();\n"
    "        CooperativeMatrix<float16,16,16,1> wb; wb.load(sb, 0, 1, 64);\n"
    "        CooperativeMatrix<float16,16,16,0> wa; wa.load(sb, 0, 0, 64);\n"
    "        CooperativeMatrix<float32,16,16,2> acc; acc.splat(0.0f);\n"
    "        acc.mma(wa, wb);\n"
    "        acc.store(c, 0, 0, n);\n"
    "    }\n"
    "}\n";
} // namespace

TEST(XpuBlockPadProbeTests, blockPadIsNotIdentity) {
    std::string padded = isaOf(kBlockPadWidthSrc, "bpw");
    std::string plain = isaOf(kPlainSharedSrc, "bpw");
    ASSERT_FALSE(padded.empty());
    ASSERT_FALSE(plain.empty());
    EXPECT_NE(padded, plain) << "block padding produced identical ISA to a plain Shared tile "
                                "(the pad map was not applied)";
}
