//
// NvptxDynamicLaneTests — `v[i]` with a runtime `i` on NVPTX
// (xpu-kernel-adaptor plan, 1.5.5.7).
//
// Why this exists. After the lut4 fix (1.5.5.4) took 11 kernels off the
// spill list, every remaining kernel the gate called "register pressure"
// turned out to be ONE shape instead. Measured across the whole llm
// library's emitted PTX, 2026-09-19:
//
//     20 kernels   a `__local_depot` written through %SP, ZERO ld.local
//     20 kernels   the portable software cooperative-matrix tile (4A.2.4)
//      1 kernel    a real ptxas register spill (q4kQ8MmqKernel, vgpr=255)
//
// The first group is not register pressure at all — `q4kMatVecKernelIL`
// spills 64 bytes at vgpr=48, with 207 registers of headroom. Its cause:
//
//     int32 sb = 0;
//     while (sb < 8) { ... acc = acc + tv[sb] * qx - uv[sb] * sx; ... }
//
// `tv` and `uv` are `Vector<float32,8>`, indexed by the loop variable.
// cajeta emits an honest `extractelement` with a dynamic index; the NVPTX
// backend legalizes THAT by writing the vector to the stack frame and
// loading one lane back. Two vectors x 8 lanes x 4 bytes = the 64 bytes
// measured, and because the loop body is too large for LLVM to unroll,
// the sixteen stores are re-executed on every iteration.
//
// Depot sizes across the group are 16, 32 and 64 bytes — one, two and
// four short vectors — which is the same dose-response the lut4 repro
// showed.
//
// The fix is a select chain: the lanes are already live SSA values in
// registers, so choosing among them costs N-1 `selp` and no memory at
// all. An out-of-range index selects `i mod N` rather than yielding
// poison, which is strictly safer than the extractelement it replaces.
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

// `dyn` mirrors q4kMatVecKernelIL's shape: the vector is BUILT by lane
// assignment (so nothing can forward it back to the load it came from) and
// then read at a runtime index. `cst` is the same kernel with a constant
// lane, the does-NOT-fire control.
const char* kSource =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void dyn(KernelBuffer<float32> out,\n"
    "            KernelBuffer<float32> xs, KernelBuffer<int32> sel,\n"
    "            uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) {\n"
    "            int64 b = (int64) i * 8L;\n"
    "            Vector<float32,8> v = xs.vload<8>(b) * 0.0f;\n"
    "            float32 d = xs[b];\n"
    "            v[0] = d * 1.0f;\n"
    "            v[1] = d * 2.0f;\n"
    "            v[2] = d * 3.0f;\n"
    "            v[3] = d * 4.0f;\n"
    "            v[4] = d * 5.0f;\n"
    "            v[5] = d * 6.0f;\n"
    "            v[6] = d * 7.0f;\n"
    "            v[7] = d * 8.0f;\n"
    "            int32 k = sel[(int64) i];\n"
    "            out[(int64) i] = v[k];\n"
    "        }\n"
    "    }\n"
    "    @Kernel\n"
    "    public static void cst(KernelBuffer<float32> out,\n"
    "            KernelBuffer<float32> xs, KernelBuffer<int32> sel,\n"
    "            uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) {\n"
    "            int64 b = (int64) i * 8L;\n"
    "            Vector<float32,8> v = xs.vload<8>(b) * 0.0f;\n"
    "            float32 d = xs[b];\n"
    "            v[0] = d * 1.0f;\n"
    "            v[1] = d * 2.0f;\n"
    "            v[2] = d * 3.0f;\n"
    "            v[3] = d * 4.0f;\n"
    "            v[4] = d * 5.0f;\n"
    "            v[5] = d * 6.0f;\n"
    "            v[6] = d * 7.0f;\n"
    "            v[7] = d * 8.0f;\n"
    "            out[(int64) i] = v[3];\n"
    "        }\n"
    "    }\n"
    "    public static int32 run() { return 1; }\n"
    "}\n";

cajeta::CajetaModulePtr compileForInspection(cajeta::Compiler& compiler,
                                             const std::string& source) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_nvptx_dynlane_" + std::to_string(rng()));
    std::filesystem::create_directories(base / "test");
    std::ofstream(base / "test" / "M.cajeta") << source;
    auto archive = std::filesystem::temp_directory_path()
                 / ("cajeta_nvptx_dynlane_arch_" + std::to_string(rng()));
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
    llvm::Module dev("nvptx_dynlane_dev", ctx);
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

// The defect, stated as a test: a runtime lane read costs no stack frame.
TEST(NvptxDynamicLaneTests, dynamicLaneLeavesNoFrameOnNvptx) {
    std::string ir, ptx, why;
    ASSERT_TRUE(lowerOne("dyn", &ir, &ptx, &why)) << why;
    EXPECT_EQ(ptx.find("__local_depot"), std::string::npos)
        << "a runtime lane read still allocates a stack frame:\n" << ptx;
    EXPECT_EQ(countOf(ptx, "[%SP"), 0u)
        << "a runtime lane read still round-trips the vector through the frame";
}

// The mechanism: an 8-lane vector takes a 7-deep select chain, no
// extractelement left for the backend to legalize.
TEST(NvptxDynamicLaneTests, dynamicLaneEmitsASelectChain) {
    std::string ir, ptx, why;
    ASSERT_TRUE(lowerOne("dyn", &ir, &ptx, &why)) << why;
    EXPECT_EQ(countOf(ir, "= select i1 %lane."), 7u)
        << "an 8-lane dynamic read must select among the lanes:\n" << ir;
}

// Does NOT fire: a constant lane is already free and must stay untouched.
// Without this the check above could pass by expanding every lane read.
TEST(NvptxDynamicLaneTests, constantLaneIsUnchanged) {
    std::string ir, ptx, why;
    ASSERT_TRUE(lowerOne("cst", &ir, &ptx, &why)) << why;
    EXPECT_EQ(countOf(ir, "= select i1 %lane."), 0u)
        << "a constant lane must not become a select chain:\n" << ir;
    EXPECT_EQ(ptx.find("__local_depot"), std::string::npos)
        << "the constant-lane control must stay frame-free";
}

// ---------------------------------------------------------------------------

namespace {

bool runOnDevice(const std::string& kernelName,
                 const std::vector<float>& xs, const std::vector<int32_t>& sel,
                 std::vector<float>* out, std::string* why) {
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

    const uint32_t n = (uint32_t) sel.size();
    auto dOut = cuda.alloc(n * sizeof(float));
    auto dXs  = cuda.alloc(xs.size() * sizeof(float));
    auto dSel = cuda.alloc(sel.size() * sizeof(int32_t));
    out->assign(n, -1.0f);
    cuda.memcpyHtoD(dOut, out->data(), n * sizeof(float));
    cuda.memcpyHtoD(dXs, (void*) xs.data(), xs.size() * sizeof(float));
    cuda.memcpyHtoD(dSel, (void*) sel.data(), sel.size() * sizeof(int32_t));
    void* params[] = { &dOut, &dXs, &dSel, (void*) &n };
    const unsigned block = 128;
    bool ok = cuda.launch(fn, (n + block - 1) / block, block, params)
              && cuda.synchronize();
    if (ok) ok = cuda.memcpyDtoH(out->data(), dOut, n * sizeof(float));
    cuda.free(dOut); cuda.free(dXs); cuda.free(dSel);
    if (!ok) { *why = "launch or copy-back failed"; return false; }
    return true;
}

} // namespace

// The load-bearing one: the select chain returns what the lane read means,
// for every lane, on the live device. `v[k] == xs[i*8] * (k+1)` by
// construction.
TEST(NvptxDynamicLaneTests, dynamicLaneMatchesTheLaneItNames) {
    if (!cajeta::xpu::nvidia::CudaDriver::available())
        GTEST_SKIP() << "no CUDA device";

    const uint32_t n = 4096;
    std::vector<float> xs(std::size_t(n) * 8);
    std::vector<int32_t> sel(n);
    std::mt19937 rng(20260919u);
    for (std::size_t i = 0; i < xs.size(); ++i)
        xs[i] = (float) ((int) (rng() % 2000) - 1000) * 0.125f;
    for (uint32_t i = 0; i < n; ++i)
        sel[i] = (int32_t) (i % 8);          // every lane, evenly

    std::string why;
    std::vector<float> got;
    ASSERT_TRUE(runOnDevice("dyn", xs, sel, &got, &why)) << why;

    std::size_t bad = 0;
    for (uint32_t i = 0; i < n; ++i) {
        const float want = xs[std::size_t(i) * 8] * (float) (sel[i] + 1);
        if (got[i] != want) {
            if (bad < 5)
                ADD_FAILURE() << "i=" << i << " lane=" << sel[i]
                              << " got " << got[i] << " want " << want;
            ++bad;
        }
    }
    EXPECT_EQ(bad, 0u) << "the select chain reads the wrong lane";
}
