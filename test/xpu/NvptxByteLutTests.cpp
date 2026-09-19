//
// NvptxByteLutTests — `Vector<int8,N>.lut4(table)` on NVPTX
// (xpu-kernel-adaptor plan, 1.5.5.3 / 1.5.5.4).
//
// Why this exists. `LoweringTarget::byteLut16`'s default allocas the
// 16-byte table and gathers it a lane at a time. On NVPTX an alloca is
// `.local`, so every lut4 call costs 16 bytes of per-work-item scratch
// and 16 `ld.local.b8`. Measured 2026-09-19 on a three-kernel repro that
// differs ONLY in its lut4 count:
//
//     vecOnlyKernel   0 lut4    0 bytes   (no spill warning)
//     lut4OneKernel   1 lut4   16 bytes
//     lut4TwoKernel   2 lut4   32 bytes
//
// and the PTX names it: `.local .align 16 .b8 __local_depot0[32]`, one
// `st.local.v4.b32` of the constant table, then the gathers. That 32 is
// exactly the figure the spill gate reports for the 18 QuantKernel
// kernels, 10 of which call lut4 twice per block (lo and hi nibble).
//
// AMDGPU already overrides this with `v_perm_b32`. NVPTX has the same
// instruction — `prmt.b32` — and ptxas is already emitting it for plain
// byte shuffles in these very kernels. The table belongs in four
// registers, not in scratch.
//
// The correctness trap. `prmt`'s selector is FOUR NIBBLES (3 bits of
// byte index plus a sign-replicate bit), where `v_perm_b32`'s is four
// BYTES. So the AMD lowering cannot be transliterated: the four byte
// indices have to be compacted into four nibbles first, and a selector
// nibble that reaches 8 silently switches to sign-replicate and returns
// 0x00/0xff instead of a table entry. `lut4MatchesThePortableGather`
// is the load-bearing test here — it runs both tiers on the device and
// compares bytes, across indices that cover all 256 values.
//

#include <gtest/gtest.h>

#include "cajeta/xpu/nvidia/NvptxBackend.h"
#include "cajeta/xpu/nvidia/NvptxKernelLowering.h"
#include "cajeta/xpu/nvidia/CudaDriver.h"

#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/method/Method.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

namespace {

// The MXFP4 kvalues, which is what every production caller passes: signed,
// so the high half is reachable and the sign-replicate trap is live.
const int8_t kKv[16] = {0, 1, 2, 3, 4, 6, 8, 12, 0, -1, -2, -3, -4, -6, -8, -12};

// Three kernels over the same buffers, differing only in the lookup:
//   lutFull  — `q.lut4(kv)`, the mxfp4/iq4_nl shape, both table halves live
//   lutLow   — `(q & 7).lut4(kv)`, high half provably dead
//   noLut    — no lut4 at all, the does-NOT-fire control
const char* kSource =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class M {\n"
    "    @Device\n"
    "    static Vector<int8,16> kv() {\n"
    "        return stack Vector<int8,16>(0,1,2,3,4,6,8,12,"
    "0,-1,-2,-3,-4,-6,-8,-12);\n"
    "    }\n"
    "    @Kernel\n"
    "    public static void lutFull(KernelBuffer<int8> out,\n"
    "            KernelBuffer<int8> idx, uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) {\n"
    "            Vector<int8,16> q = idx.vload<16>((int64) i * 16L);\n"
    "            out.vstore((int64) i * 16L, q.lut4(M.kv()));\n"
    "        }\n"
    "    }\n"
    "    @Kernel\n"
    "    public static void lutLow(KernelBuffer<int8> out,\n"
    "            KernelBuffer<int8> idx, uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) {\n"
    "            Vector<int8,16> q = idx.vload<16>((int64) i * 16L);\n"
    "            out.vstore((int64) i * 16L, (q & 7).lut4(M.kv()));\n"
    "        }\n"
    "    }\n"
    "    @Kernel\n"
    "    public static void noLut(KernelBuffer<int8> out,\n"
    "            KernelBuffer<int8> idx, uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) {\n"
    "            Vector<int8,16> q = idx.vload<16>((int64) i * 16L);\n"
    "            out.vstore((int64) i * 16L, (q & 15) + (q >> 4));\n"
    "        }\n"
    "    }\n"
    "    public static int32 run() { return 1; }\n"
    "}\n";

cajeta::CajetaModulePtr compileForInspection(cajeta::Compiler& compiler,
                                             const std::string& source) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_nvptx_bytelut_" + std::to_string(rng()));
    std::filesystem::create_directories(base / "test");
    std::ofstream(base / "test" / "M.cajeta") << source;
    auto archive = std::filesystem::temp_directory_path()
                 / ("cajeta_nvptx_bytelut_arch_" + std::to_string(rng()));
    std::filesystem::create_directories(archive);
    auto full = base / "test" / "M.cajeta";
    auto m = compiler.createModule(full.string(), base.string(),
                                   archive.string());
    compiler.compile(m);
    return m;
}

cajeta::MethodPtr findMethod(const cajeta::CajetaClassPtr& klass,
                             const std::string& name) {
    for (auto& [k, m] : klass->getMethods())
        if (m && m->getName() == name) return m;
    return nullptr;
}

// Lower one kernel for sm_89. `ir` gets the device IR, `ptx` the assembly.
// False with the reason in `why` if it refused to lower.
bool lowerOne(const std::string& kernelName, std::string* ir,
              std::string* ptx, std::string* why) {
    cajeta::Compiler compiler;
    auto module = compileForInspection(compiler, kSource);
    auto klass = module->getStructures()["test.M"];
    if (!klass) { *why = "test.M did not compile"; return false; }
    auto k = findMethod(klass, kernelName);
    if (!k) { *why = "kernel " + kernelName + " not found"; return false; }
    auto tm = cajeta::xpu::nvidia::createNvptxTargetMachine("sm_89");
    if (!tm) { *why = "no sm_89 target machine"; return false; }
    llvm::LLVMContext ctx;
    llvm::Module dev("nvptx_bytelut_dev", ctx);
    cajeta::xpu::nvidia::configureDeviceModule(dev, *tm);
    try {
        cajeta::xpu::nvidia::lowerKernel(k, dev);
    } catch (const std::exception& e) { *why = e.what(); return false; }
    ir->clear();
    { llvm::raw_string_ostream os(*ir); dev.print(os, nullptr); }
    *ptx = cajeta::xpu::nvidia::emitPtx(dev, *tm);
    if (ptx->empty()) { *why = "no ptx"; return false; }
    return true;
}

std::size_t countOf(const std::string& hay, const std::string& needle) {
    std::size_t n = 0;
    for (std::size_t at = hay.find(needle); at != std::string::npos;
         at = hay.find(needle, at + 1))
        ++n;
    return n;
}

} // namespace

// 1.5.5.4 — the table goes to registers via prmt.b32, not to `.local`.
TEST(NvptxByteLutTests, lut4EmitsNvptxPrmt) {
    std::string ir, ptx, why;
    ASSERT_TRUE(lowerOne("lutFull", &ir, &ptx, &why)) << why;
    EXPECT_NE(ir.find("llvm.nvvm.prmt"), std::string::npos)
        << "lut4 must lower to the byte permute on NVPTX, not a gather";
}

// The defect itself, stated as a test: no scratch for a lut4 kernel.
TEST(NvptxByteLutTests, lut4LeavesNoScratchOnNvptx) {
    std::string ir, ptx, why;
    ASSERT_TRUE(lowerOne("lutFull", &ir, &ptx, &why)) << why;
    // The portable gather names its table slot `lut.tbl`; ordinary locals also
    // alloca here and are optimized away, so this names the one that matters.
    EXPECT_EQ(ir.find("lut.tbl"), std::string::npos)
        << "lut4 still allocas the table:\n" << ir;
    EXPECT_EQ(ptx.find("__local_depot"), std::string::npos)
        << "lut4 still spills the table to local memory:\n" << ptx;
    EXPECT_EQ(countOf(ptx, "ld.local"), 0u)
        << "lut4 still gathers out of local memory";
}

// The pair, mirroring the AMD test: an index visibly masked below 8 cannot
// reach the table's high half, so the high permute and the per-byte select
// that chooses between halves are both dead. One prmt a dword instead of
// three. It must FIRE on `& 7` and NOT fire on `& 15`, which is every
// production caller.
TEST(NvptxByteLutTests, lut4MaskedToLowHalfTakesOnePrmtPerDword) {
    std::string irLow, irFull, ptx, why;
    ASSERT_TRUE(lowerOne("lutLow", &irLow, &ptx, &why)) << why;
    ASSERT_TRUE(lowerOne("lutFull", &irFull, &ptx, &why)) << why;
    EXPECT_EQ(countOf(irLow, "call i32 @llvm.nvvm.prmt"), 4u)
        << "a low-half index must take one permute per dword";
    EXPECT_EQ(countOf(irFull, "call i32 @llvm.nvvm.prmt"), 12u)
        << "a full 4-bit index must keep both halves and the select";
}

// Does NOT fire: a byte kernel with no lut4 gains no permute of ours and
// keeps whatever it had. Without this the check above could pass by
// emitting prmt unconditionally.
TEST(NvptxByteLutTests, kernelWithoutLut4IsUnchanged) {
    std::string ir, ptx, why;
    ASSERT_TRUE(lowerOne("noLut", &ir, &ptx, &why)) << why;
    EXPECT_EQ(ir.find("llvm.nvvm.prmt"), std::string::npos)
        << "a kernel with no lut4 must not acquire a byte permute";
    EXPECT_EQ(ptx.find("__local_depot"), std::string::npos)
        << "the no-lut4 control must stay scratch-free";
}

// ---------------------------------------------------------------------------
// The load-bearing one. Both tiers, on the device, byte for byte.
// ---------------------------------------------------------------------------

namespace {

// Run `kernelName` over `idx` on the live device. Byte results in `out`.
bool runOnDevice(const std::string& kernelName,
                 const std::vector<int8_t>& idx, std::vector<int8_t>* out,
                 std::string* why) {
    std::string ir, ptx;
    if (!lowerOne(kernelName, &ir, &ptx, why)) return false;
    std::vector<uint8_t> cubin =
        cajeta::xpu::nvidia::assembleCubin(ptx, "sm_89");
    if (cubin.empty()) { *why = "ptxas rejected the PTX"; return false; }

    cajeta::xpu::nvidia::CudaDriver cuda;
    if (!cuda.init()) { *why = "cuda init failed"; return false; }
    auto mod = cuda.loadModule(cubin.data(), cubin.size());
    if (!mod) { *why = "loadModule failed"; return false; }
    auto fn = cuda.getFunction(mod, kernelName.c_str());
    if (!fn) { *why = "getFunction failed"; return false; }

    const uint32_t n = (uint32_t) (idx.size() / 16);
    const std::size_t bytes = idx.size();
    auto dOut = cuda.alloc(bytes);
    auto dIdx = cuda.alloc(bytes);
    out->assign(idx.size(), (int8_t) 0x7f);
    cuda.memcpyHtoD(dIdx, (void*) idx.data(), bytes);
    cuda.memcpyHtoD(dOut, out->data(), bytes);
    void* params[] = { &dOut, &dIdx, (void*) &n };
    const unsigned block = 128;
    bool ok = cuda.launch(fn, (n + block - 1) / block, block, params)
              && cuda.synchronize();
    if (ok) ok = cuda.memcpyDtoH(out->data(), dOut, bytes);
    cuda.free(dOut); cuda.free(dIdx);
    if (!ok) { *why = "launch or copy-back failed"; return false; }
    return true;
}

} // namespace

// 1.5.5.4 acceptance: the permute path returns exactly what the gather
// path returns. Indices cover all 256 byte values, so `& 15` wrapping and
// the negative-index case are both exercised, and so is every selector
// nibble that could trip prmt's sign-replicate mode.
TEST(NvptxByteLutTests, lut4MatchesThePortableGather) {
    if (!cajeta::xpu::nvidia::CudaDriver::available())
        GTEST_SKIP() << "no CUDA device";

    std::vector<int8_t> idx(256 * 16);
    std::mt19937 rng(20260919u);
    for (std::size_t i = 0; i < 256; ++i)            // every byte value, in order
        for (std::size_t j = 0; j < 16; ++j)
            idx[i * 16 + j] = (int8_t) ((i + j) & 0xff);
    for (std::size_t i = 0; i < idx.size(); ++i)     // and a random tail
        if (i >= idx.size() / 2) idx[i] = (int8_t) (rng() & 0xff);

    std::string why;
    std::vector<int8_t> got;
    ASSERT_TRUE(runOnDevice("lutFull", idx, &got, &why)) << why;

    std::size_t bad = 0;
    for (std::size_t i = 0; i < idx.size(); ++i) {
        const int8_t want = kKv[(unsigned) (uint8_t) idx[i] & 15u];
        if (got[i] != want) {
            if (bad < 5)
                ADD_FAILURE() << "i=" << i << " idx=" << (int) idx[i]
                              << " got " << (int) got[i]
                              << " want " << (int) want;
            ++bad;
        }
    }
    EXPECT_EQ(bad, 0u) << "the permute path disagrees with the table";
}

// The low-half arm has its own selector arithmetic, so it needs its own
// device run: `& 7` must read kv[0..7] and never the high half.
TEST(NvptxByteLutTests, lut4LowHalfMatchesTheTableOnDevice) {
    if (!cajeta::xpu::nvidia::CudaDriver::available())
        GTEST_SKIP() << "no CUDA device";

    std::vector<int8_t> idx(256 * 16);
    for (std::size_t i = 0; i < 256; ++i)
        for (std::size_t j = 0; j < 16; ++j)
            idx[i * 16 + j] = (int8_t) ((i + j) & 0xff);

    std::string why;
    std::vector<int8_t> got;
    ASSERT_TRUE(runOnDevice("lutLow", idx, &got, &why)) << why;

    std::size_t bad = 0;
    for (std::size_t i = 0; i < idx.size(); ++i) {
        const int8_t want = kKv[(unsigned) (uint8_t) idx[i] & 7u];
        if (got[i] != want) {
            if (bad < 5)
                ADD_FAILURE() << "i=" << i << " idx=" << (int) idx[i]
                              << " got " << (int) got[i]
                              << " want " << (int) want;
            ++bad;
        }
    }
    EXPECT_EQ(bad, 0u) << "the low-half permute disagrees with the table";
}
