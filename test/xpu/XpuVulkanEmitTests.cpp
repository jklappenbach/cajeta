//
// CajetaXPU Vulkan bring-up — SPIR-V emission + kernel-body lowering through
// the SHARED AST walk with the SPIR-V (Vulkan) LoweringTarget.
//
// This is the third backend's seam payoff — and the measured proof that Vulkan
// forks MORE than AMD did (cajeta-xpu-matrix.md): the SAME shared AST walk
// lowers the kernel body, but the SPIR-V LoweringTarget forks the kernel
// SIGNATURE (a `void main()` GLCompute entry, no params) and BUFFER ACCESS
// (descriptor-set storage buffers via resource.handlefrombinding/getpointer,
// since LLVM 23's SPIR-V backend has no raw-pointer kernel ABI and no BDA).
//
// Two assertion layers:
//   - IR structure (deterministic): hlsl.shader compute marker, the SPIR-V
//     coordinate intrinsics, resource.handlefrombinding + getpointer.
//   - SPIR-V smoke: end-to-end codegen produces a Vulkan compute module
//     (OpEntryPoint GLCompute, LocalSize, DescriptorSet/Binding), and — when
//     spirv-val is installed — a structurally VALID Vulkan 1.3 binary.
//
// GPU-free (SPIR-V is just bytes; spirv-val is a host tool). On-device
// verification is XpuSaxpyVulkanDeviceTests.
//

#include "gtest/gtest.h"

#include "cajeta/xpu/vulkan/SpirvBackend.h"
#include "cajeta/xpu/vulkan/SpirvKernelLowering.h"

#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/method/Method.h"
#include "cajeta/error/Exception.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

using namespace cajeta::xpu::vulkan;
using cajeta::Compiler;
using cajeta::CajetaModulePtr;

namespace {

CajetaModulePtr compileForInspection(Compiler& compiler,
                                     const std::string& source,
                                     const std::string& fqClassName) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_xpu_vulkan_" + std::to_string(rng()));
    std::filesystem::create_directories(base);
    std::filesystem::path rel;
    size_t start = 0;
    for (size_t i = 0; i <= fqClassName.size(); ++i) {
        if (i == fqClassName.size() || fqClassName[i] == '.') {
            rel /= fqClassName.substr(start, i - start);
            start = i + 1;
        }
    }
    rel += ".cajeta";
    auto full = base / rel;
    std::filesystem::create_directories(full.parent_path());
    std::ofstream out(full); out << source; out.close();
    auto archive = std::filesystem::temp_directory_path()
                 / ("cajeta_xpu_vulkan_arch_" + std::to_string(rng()));
    std::filesystem::create_directories(archive);
    auto m = compiler.createModule(full.string(), base.string(), archive.string());
    compiler.compile(m);
    return m;
}

cajeta::MethodPtr findMethod(const cajeta::CajetaClassPtr& klass,
                             const std::string& name) {
    for (auto& [k, m] : klass->getMethods()) {
        if (m && m->getName() == name) return m;
    }
    return nullptr;
}

std::string printModule(llvm::Module& m) {
    std::string s;
    llvm::raw_string_ostream os(s);
    m.print(os, nullptr);
    return os.str();
}

// Run spirv-val on a SPIR-V binary if the tool is installed; returns true if
// the module is valid, false if invalid, and std::nullopt if the tool is
// absent (so the caller can skip rather than fail).
std::optional<bool> validateSpirv(const std::vector<uint8_t>& spirv) {
    auto tool = llvm::sys::findProgramByName("spirv-val");
    if (!tool) return std::nullopt;
    static std::mt19937_64 rng(std::random_device{}());
    auto path = std::filesystem::temp_directory_path()
              / ("cajeta_spv_" + std::to_string(rng()) + ".spv");
    {
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(spirv.data()),
                  (std::streamsize) spirv.size());
    }
    llvm::StringRef env = "--target-env";
    llvm::StringRef ver = "vulkan1.3";
    // path::c_str() is const wchar_t* on Windows; go through string() so
    // the StringRef has a char buffer to bind to (held until after the wait).
    std::string fileStr = path.string();
    llvm::StringRef file = fileStr;
    llvm::SmallVector<llvm::StringRef, 4> args = {*tool, env, ver, file};
    int rc = llvm::sys::ExecuteAndWait(*tool, args);
    std::filesystem::remove(path);
    return rc == 0;
}

// Scan a SPIR-V binary for `OpDecorate <id> SpecId <specId>` (opcode 71, deco 1)
// — direct proof that a genuine specialization constant with that SpecId is
// present, as opposed to a baked literal (which carries no SpecId decoration).
bool hasSpecIdDecoration(const std::vector<uint8_t>& spirv, uint32_t specId) {
    if (spirv.size() < 20 || (spirv.size() % 4) != 0) return false;
    std::vector<uint32_t> w(spirv.size() / 4);
    std::memcpy(w.data(), spirv.data(), spirv.size());
    if (w[0] != 0x07230203u) return false;
    for (size_t i = 5; i < w.size();) {
        uint32_t wc = w[i] >> 16, op = w[i] & 0xFFFFu;
        if (wc == 0 || i + wc > w.size()) break;
        if (op == 71u /*OpDecorate*/ && wc == 4 && w[i + 2] == 1u /*SpecId*/ &&
            w[i + 3] == specId)
            return true;
        i += wc;
    }
    return false;
}

} // namespace

// The spirv target is registered in this LLVM build and yields a usable
// TargetMachine for the Vulkan 1.3 compute env.
TEST(XpuVulkanEmitTests, spirvTargetMachineAvailable) {
    auto tm = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tm, nullptr) << "spirv target not built into this LLVM";
}

// SAXPY lowers through the shared walk + SPIR-V target into a valid Vulkan
// compute module. The IR carries the Vulkan-specific seam decisions (no
// function params, hlsl compute marker, descriptor resource access); the
// SPIR-V text carries the Vulkan structure; spirv-val confirms validity.
TEST(XpuVulkanEmitTests, lowersSaxpyToSpirv) {
    auto src =
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.KernelThread;\n"
        "public class M {\n"
        "    @Kernel\n"
        "    public static void saxpy(KernelBuffer<float32> y, KernelBuffer<float32> x,\n"
        "                              float32 a, uint32 n) {\n"
        "        uint32 i = KernelThread.globalIdX();\n"
        "        if (i < n) {\n"
        "            y[i] = a * x[i] + y[i];\n"
        "        }\n"
        "    }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.M");
    auto klass = module->getStructures()["test.M"];
    ASSERT_NE(klass, nullptr);
    auto saxpy = findMethod(klass, "saxpy");
    ASSERT_NE(saxpy, nullptr);

    // NOTE: SPIR-V emission MUTATES the device module in place (the structurizer
    // rewrites the CFG into structured form), so each artifact lowers into its
    // own fresh module — never emit twice from one module. AND a fresh
    // TargetMachine per artifact: LLVM's SPIR-V backend accumulates codegen state
    // (SPIRVGlobalRegistry) on the TM, so reusing one TM across two emits corrupts
    // it (the production VulkanRegistration path uses a fresh TM per kernel for
    // exactly this reason).
    auto tmIr = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tmIr, nullptr);
    llvm::LLVMContext irCtx;
    llvm::Module irModule("xpu_saxpy_vulkan_ir", irCtx);
    configureDeviceModule(irModule, *tmIr);
    llvm::Function* fn = lowerKernel(saxpy, irModule);
    ASSERT_NE(fn, nullptr);
    // Seam: the Vulkan compute entry takes NO parameters (args arrive via
    // descriptors), unlike the NVPTX/AMDGPU pointer-arg kernels.
    EXPECT_EQ(fn->arg_size(), 0u);

    std::string ir = printModule(irModule);
    // Seam: HLSL compute marker (drives OpEntryPoint GLCompute + LocalSize).
    EXPECT_NE(ir.find("hlsl.shader"), std::string::npos) << ir;
    // Seam: global id is a native SPIR-V intrinsic (overrides the default).
    EXPECT_NE(ir.find("llvm.spv.thread.id"), std::string::npos) << ir;
    // Seam: descriptor-set buffer access — handle from binding + getpointer.
    EXPECT_NE(ir.find("llvm.spv.resource.handlefrombinding"),
              std::string::npos) << ir;
    EXPECT_NE(ir.find("llvm.spv.resource.getpointer"), std::string::npos) << ir;

    auto tmText = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tmText, nullptr);
    llvm::LLVMContext textCtx;
    llvm::Module textModule("xpu_saxpy_vulkan_text", textCtx);
    configureDeviceModule(textModule, *tmText);
    lowerKernel(saxpy, textModule);
    std::string text = emitSpirvText(textModule, *tmText);
    ASSERT_FALSE(text.empty()) << "SPIR-V text emission produced nothing";
    EXPECT_NE(text.find("OpCapability Shader"), std::string::npos) << text;
    EXPECT_NE(text.find("OpEntryPoint GLCompute"), std::string::npos) << text;
    EXPECT_NE(text.find("LocalSize"), std::string::npos) << text;
    EXPECT_NE(text.find("DescriptorSet"), std::string::npos) << text;
    EXPECT_NE(text.find("Binding"), std::string::npos) << text;

    auto tmBin = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tmBin, nullptr);
    llvm::LLVMContext binCtx;
    llvm::Module binModule("xpu_saxpy_vulkan_bin", binCtx);
    configureDeviceModule(binModule, *tmBin);
    lowerKernel(saxpy, binModule);
    std::vector<uint8_t> spirv = emitSpirv(binModule, *tmBin);
    ASSERT_FALSE(spirv.empty()) << "SPIR-V binary emission produced nothing";
    // SPIR-V magic number 0x07230203 (little-endian first word).
    ASSERT_GE(spirv.size(), 4u);
    EXPECT_EQ(spirv[0], 0x03u);
    EXPECT_EQ(spirv[1], 0x02u);
    EXPECT_EQ(spirv[2], 0x23u);
    EXPECT_EQ(spirv[3], 0x07u);

    if (auto valid = validateSpirv(spirv)) {
        EXPECT_TRUE(*valid) << "spirv-val rejected the emitted Vulkan module";
    } else {
        GTEST_SUCCEED() << "spirv-val not installed; skipped binary validation";
    }
}

// Item 7: a POD struct passed by value as a kernel arg lowers to a valid Vulkan
// compute module. On SPIR-V the struct rides its own descriptor-bound storage
// buffer (the scalar-SSBO mechanism, element type = the struct); the kernel
// reads p.mul / p.add to compute out[i] = i*mul + add.
TEST(XpuVulkanEmitTests, lowersPodStructArgToSpirv) {
    auto src =
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.KernelThread;\n"
        "public class Params {\n"
        "    int32 mul;\n"
        "    int32 add;\n"
        "    public Params(int32 mul, int32 add)"
        " { this.mul = mul; this.add = add; }\n"
        "}\n"
        "public class M {\n"
        "    @Kernel\n"
        "    public static void k(KernelBuffer<int32> out, Params p) {\n"
        "        uint32 i = KernelThread.globalIdX();\n"
        "        out[i] = (int32)i * p.mul + p.add;\n"
        "    }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.M");
    auto k = findMethod(module->getStructures()["test.M"], "k");
    ASSERT_NE(k, nullptr);

    auto tmText = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tmText, nullptr);
    llvm::LLVMContext textCtx;
    llvm::Module textModule("xpu_podstruct_vulkan_text", textCtx);
    configureDeviceModule(textModule, *tmText);
    llvm::Function* fn = lowerKernel(k, textModule);
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn->arg_size(), 0u);   // args arrive via descriptors
    std::string text = emitSpirvText(textModule, *tmText);
    ASSERT_FALSE(text.empty());
    EXPECT_NE(text.find("OpEntryPoint GLCompute"), std::string::npos) << text;

    auto tmBin = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tmBin, nullptr);
    llvm::LLVMContext binCtx;
    llvm::Module binModule("xpu_podstruct_vulkan_bin", binCtx);
    configureDeviceModule(binModule, *tmBin);
    lowerKernel(k, binModule);
    std::vector<uint8_t> spirv = emitSpirv(binModule, *tmBin);
    ASSERT_FALSE(spirv.empty());
    if (auto valid = validateSpirv(spirv)) {
        EXPECT_TRUE(*valid) << "spirv-val rejected the POD-struct-arg module";
    } else {
        GTEST_SUCCEED() << "spirv-val not installed; skipped binary validation";
    }
}

// A strided-sum loop kernel (identical source to the NVPTX/AMD loop tests)
// lowers to a valid Vulkan compute SPIR-V module with descriptor buffers.
TEST(XpuVulkanEmitTests, lowersStridedSumLoop) {
    auto src =
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.KernelThread;\n"
        "public class M {\n"
        "    @Kernel\n"
        "    public static void strideSum(KernelBuffer<int32> out, KernelBuffer<int32> in,\n"
        "                                  uint32 n, uint32 stride) {\n"
        "        uint32 i = KernelThread.globalIdX();\n"
        "        int32 acc = 0;\n"
        "        for (uint32 j = i; j < n; j += stride) {\n"
        "            acc += in[j];\n"
        "        }\n"
        "        out[i] = acc;\n"
        "    }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.M");
    auto strideSum = findMethod(module->getStructures()["test.M"], "strideSum");
    ASSERT_NE(strideSum, nullptr);

    // Fresh module AND fresh TargetMachine per artifact: emission mutates the
    // module, and the SPIR-V backend accumulates SPIRVGlobalRegistry state on the
    // TM, so reusing one TM across two emits corrupts it (see lowersSaxpyToSpirv).
    auto tmText = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tmText, nullptr);
    llvm::LLVMContext textCtx;
    llvm::Module textModule("xpu_stridesum_vulkan_text", textCtx);
    configureDeviceModule(textModule, *tmText);
    llvm::Function* fn = lowerKernel(strideSum, textModule);
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn->arg_size(), 0u);
    std::string text = emitSpirvText(textModule, *tmText);
    ASSERT_FALSE(text.empty());
    EXPECT_NE(text.find("OpEntryPoint GLCompute"), std::string::npos) << text;

    auto tmBin = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tmBin, nullptr);
    llvm::LLVMContext binCtx;
    llvm::Module binModule("xpu_stridesum_vulkan_bin", binCtx);
    configureDeviceModule(binModule, *tmBin);
    lowerKernel(strideSum, binModule);
    std::vector<uint8_t> spirv = emitSpirv(binModule, *tmBin);
    ASSERT_FALSE(spirv.empty());
    if (auto valid = validateSpirv(spirv)) {
        EXPECT_TRUE(*valid) << "spirv-val rejected the strided-sum module";
    }
}

// Item 8 Stage B: a Texture2D.sample(sampler, u, v) kernel lowers to native
// Vulkan image sampling — the texture binds a SAMPLED_IMAGE descriptor, the
// sampler a SAMPLER descriptor, and the sample becomes OpImageSampleExplicitLod
// with an explicit Lod (compute-valid; no implicit derivatives). Proven against
// strict spirv-val --target-env vulkan1.3.
TEST(XpuVulkanEmitTests, lowersTextureSampleToSpirv) {
    auto src =
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.KernelThread;\n"
        "import cajeta.gfx.Texture2D;\n"
        "import cajeta.gfx.Sampler;\n"
        "public class M {\n"
        "    @Kernel\n"
        "    public static void sampleTex(Texture2D tex, Sampler s,\n"
        "                                 KernelBuffer<float32> out, uint32 n) {\n"
        "        uint32 i = KernelThread.globalIdX();\n"
        "        if (i < n) {\n"
        "            Vector<float32,4> c = tex.sample(s, 0.5, 0.5); out[i] = c.x;\n"
        "        }\n"
        "    }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.M");
    auto k = findMethod(module->getStructures()["test.M"], "sampleTex");
    ASSERT_NE(k, nullptr);

    auto tmIr = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tmIr, nullptr);
    llvm::LLVMContext irCtx;
    llvm::Module irModule("xpu_texsample_vulkan_ir", irCtx);
    configureDeviceModule(irModule, *tmIr);
    llvm::Function* fn = lowerKernel(k, irModule);
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn->arg_size(), 0u);   // args arrive via descriptors

    std::string ir = printModule(irModule);
    // Image + sampler bound as descriptors; sampled via samplelevel.
    EXPECT_NE(ir.find("llvm.spv.resource.handlefrombinding"),
              std::string::npos) << ir;
    EXPECT_NE(ir.find("llvm.spv.resource.samplelevel"), std::string::npos) << ir;

    auto tmText = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tmText, nullptr);
    llvm::LLVMContext textCtx;
    llvm::Module textModule("xpu_texsample_vulkan_text", textCtx);
    configureDeviceModule(textModule, *tmText);
    lowerKernel(k, textModule);
    std::string text = emitSpirvText(textModule, *tmText);
    ASSERT_FALSE(text.empty());
    EXPECT_NE(text.find("OpEntryPoint GLCompute"), std::string::npos) << text;
    EXPECT_NE(text.find("OpTypeImage"), std::string::npos) << text;
    EXPECT_NE(text.find("OpTypeSampler"), std::string::npos) << text;
    EXPECT_NE(text.find("OpImageSampleExplicitLod"), std::string::npos) << text;

    auto tmBin = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tmBin, nullptr);
    llvm::LLVMContext binCtx;
    llvm::Module binModule("xpu_texsample_vulkan_bin", binCtx);
    configureDeviceModule(binModule, *tmBin);
    lowerKernel(k, binModule);
    std::vector<uint8_t> spirv = emitSpirv(binModule, *tmBin);
    ASSERT_FALSE(spirv.empty());
    if (auto valid = validateSpirv(spirv)) {
        EXPECT_TRUE(*valid) << "spirv-val rejected the texture-sample module";
    } else {
        GTEST_SUCCEED() << "spirv-val not installed; skipped binary validation";
    }
}

// texelFetch: Texture2D.fetch(x, y) reads the exact integer texel of the SAMPLED
// image (no sampler), lowering to OpImageFetch (+ a Lod operand) via the fork
// llvm.spv.resource.load.level intrinsic. Distinct from Image2D.load's
// OpImageRead: the sampled image is Sampled=1 so the backend's read/fetch
// selection picks Fetch. GPU-free proof it is spirv-val-clean under strict
// spirv-val --target-env vulkan1.3.
TEST(XpuVulkanEmitTests, lowersTextureFetchToSpirv) {
    auto src =
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.KernelThread;\n"
        "import cajeta.gfx.Texture2D;\n"
        "public class M {\n"
        "    @Kernel\n"
        "    public static void fetchTex(Texture2D tex,\n"
        "                                KernelBuffer<float32> out, uint32 n) {\n"
        "        uint32 i = KernelThread.globalIdX();\n"
        "        if (i < n) {\n"
        "            Vector<float32,4> c = tex.fetch(i, 0); out[i] = c.x;\n"
        "        }\n"
        "    }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.M");
    auto k = findMethod(module->getStructures()["test.M"], "fetchTex");
    ASSERT_NE(k, nullptr);

    auto tmIr = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tmIr, nullptr);
    llvm::LLVMContext irCtx;
    llvm::Module irModule("xpu_texfetch_vulkan_ir", irCtx);
    configureDeviceModule(irModule, *tmIr);
    llvm::Function* fn = lowerKernel(k, irModule);
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn->arg_size(), 0u);   // args arrive via descriptors

    std::string ir = printModule(irModule);
    // Image bound as a descriptor; fetched (unfiltered) via load.level. NO
    // sampler descriptor or samplelevel — fetch has no Sampler.
    EXPECT_NE(ir.find("llvm.spv.resource.handlefrombinding"),
              std::string::npos) << ir;
    EXPECT_NE(ir.find("llvm.spv.resource.load.level"), std::string::npos) << ir;
    EXPECT_EQ(ir.find("llvm.spv.resource.samplelevel"), std::string::npos) << ir;

    auto tmText = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tmText, nullptr);
    llvm::LLVMContext textCtx;
    llvm::Module textModule("xpu_texfetch_vulkan_text", textCtx);
    configureDeviceModule(textModule, *tmText);
    lowerKernel(k, textModule);
    std::string text = emitSpirvText(textModule, *tmText);
    ASSERT_FALSE(text.empty());
    EXPECT_NE(text.find("OpEntryPoint GLCompute"), std::string::npos) << text;
    EXPECT_NE(text.find("OpTypeImage"), std::string::npos) << text;
    EXPECT_NE(text.find("OpImageFetch"), std::string::npos) << text;
    EXPECT_EQ(text.find("OpTypeSampler"), std::string::npos) << text;

    auto tmBin = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tmBin, nullptr);
    llvm::LLVMContext binCtx;
    llvm::Module binModule("xpu_texfetch_vulkan_bin", binCtx);
    configureDeviceModule(binModule, *tmBin);
    lowerKernel(k, binModule);
    std::vector<uint8_t> spirv = emitSpirv(binModule, *tmBin);
    ASSERT_FALSE(spirv.empty());
    if (auto valid = validateSpirv(spirv)) {
        EXPECT_TRUE(*valid) << "spirv-val rejected the texture-fetch module";
    } else {
        GTEST_SUCCEED() << "spirv-val not installed; skipped binary validation";
    }
}

// B3 Step 2b: integer texelFetch. A Texture2D<int32> binds an INTEGER sampled
// image (spirv.Image sampled-type i32) and OpImageFetch yields a <4 x i32> texel
// — the integer-image path, distinct from the float fetch above (no convert).
TEST(XpuVulkanEmitTests, lowersIntTextureFetchToSpirv) {
    auto src =
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.KernelThread;\n"
        "import cajeta.gfx.Texture2D;\n"
        "public class M {\n"
        "    @Kernel\n"
        "    public static void fetchTex(Texture2D<int32> tex,\n"
        "                                KernelBuffer<int32> out, uint32 n) {\n"
        "        uint32 i = KernelThread.globalIdX();\n"
        "        if (i < n) {\n"
        "            Vector<int32,4> c = tex.fetch(i, 0); out[i] = c.x;\n"
        "        }\n"
        "    }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.M");
    auto k = findMethod(module->getStructures()["test.M"], "fetchTex");
    ASSERT_NE(k, nullptr);

    auto tmIr = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tmIr, nullptr);
    llvm::LLVMContext irCtx;
    llvm::Module irModule("xpu_texfetch_int_vulkan_ir", irCtx);
    configureDeviceModule(irModule, *tmIr);
    llvm::Function* fn = lowerKernel(k, irModule);
    ASSERT_NE(fn, nullptr);

    std::string ir = printModule(irModule);
    // Integer fetch: the load.level result is <4 x i32> (vs <4 x float> for the
    // float texture), proving the texel type threaded through to the seam.
    EXPECT_NE(ir.find("llvm.spv.resource.load.level"), std::string::npos) << ir;
    EXPECT_NE(ir.find("<4 x i32>"), std::string::npos) << ir;

    auto tmText = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tmText, nullptr);
    llvm::LLVMContext textCtx;
    llvm::Module textModule("xpu_texfetch_int_vulkan_text", textCtx);
    configureDeviceModule(textModule, *tmText);
    lowerKernel(k, textModule);
    std::string text = emitSpirvText(textModule, *tmText);
    ASSERT_FALSE(text.empty());
    EXPECT_NE(text.find("OpImageFetch"), std::string::npos) << text;
    EXPECT_EQ(text.find("OpTypeSampler"), std::string::npos) << text;

    auto tmBin = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tmBin, nullptr);
    llvm::LLVMContext binCtx;
    llvm::Module binModule("xpu_texfetch_int_vulkan_bin", binCtx);
    configureDeviceModule(binModule, *tmBin);
    lowerKernel(k, binModule);
    std::vector<uint8_t> spirv = emitSpirv(binModule, *tmBin);
    ASSERT_FALSE(spirv.empty());
    if (auto valid = validateSpirv(spirv)) {
        EXPECT_TRUE(*valid) << "spirv-val rejected the integer texture-fetch module";
    } else {
        GTEST_SUCCEED() << "spirv-val not installed; skipped binary validation";
    }
}

// B3 texture dims: Texture3D fetch lowers to OpImageFetch on a 3-D image
// (OpTypeImage … 3D), via load.level with a <3 x i32> coord. spirv-val confirms
// the 3-D sampled-image module is well-formed.
TEST(XpuVulkanEmitTests, lowers3dTextureFetchToSpirv) {
    auto src =
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.KernelThread;\n"
        "import cajeta.gfx.Texture3D;\n"
        "public class M {\n"
        "    @Kernel\n"
        "    public static void fetchVol(Texture3D vol,\n"
        "                                KernelBuffer<float32> out, uint32 n) {\n"
        "        uint32 i = KernelThread.globalIdX();\n"
        "        if (i < n) {\n"
        "            Vector<float32,4> c = vol.fetch(i, 0, 0); out[i] = c.x;\n"
        "        }\n"
        "    }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.M");
    auto k = findMethod(module->getStructures()["test.M"], "fetchVol");
    ASSERT_NE(k, nullptr);

    auto tmText = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tmText, nullptr);
    llvm::LLVMContext textCtx;
    llvm::Module textModule("xpu_tex3d_vulkan_text", textCtx);
    configureDeviceModule(textModule, *tmText);
    lowerKernel(k, textModule);
    std::string text = emitSpirvText(textModule, *tmText);
    ASSERT_FALSE(text.empty());
    EXPECT_NE(text.find("OpImageFetch"), std::string::npos) << text;
    EXPECT_NE(text.find(" 3D "), std::string::npos) << text;  // OpTypeImage … 3D
    EXPECT_EQ(text.find("OpTypeSampler"), std::string::npos) << text;

    auto tmBin = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tmBin, nullptr);
    llvm::LLVMContext binCtx;
    llvm::Module binModule("xpu_tex3d_vulkan_bin", binCtx);
    configureDeviceModule(binModule, *tmBin);
    lowerKernel(k, binModule);
    std::vector<uint8_t> spirv = emitSpirv(binModule, *tmBin);
    ASSERT_FALSE(spirv.empty());
    if (auto valid = validateSpirv(spirv)) {
        EXPECT_TRUE(*valid) << "spirv-val rejected the 3-D texture-fetch module";
    } else {
        GTEST_SUCCEED() << "spirv-val not installed; skipped binary validation";
    }
}

// B3 Step 2b: integer textures are FETCH-ONLY. Lowering a kernel that calls
// .sample() on a Texture2D<int32> throws (XPU-N01) — the hardware texture sampler
// cannot filter integer texels, so this is rejected at the call site, steering
// the user to fetch(). The float-only-sample contract, enforced at compile time.
TEST(XpuVulkanEmitTests, integerTextureSampleRejected) {
    auto src =
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.KernelThread;\n"
        "import cajeta.gfx.Texture2D;\n"
        "import cajeta.gfx.Sampler;\n"
        "public class M {\n"
        "    @Kernel\n"
        "    public static void sampleTex(Texture2D<int32> tex, Sampler s,\n"
        "                                 KernelBuffer<int32> out, uint32 n) {\n"
        "        uint32 i = KernelThread.globalIdX();\n"
        "        if (i < n) {\n"
        "            Vector<int32,4> c = tex.sample(s, 0.0f, 0.0f); out[i] = c.x;\n"
        "        }\n"
        "    }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.M");
    auto k = findMethod(module->getStructures()["test.M"], "sampleTex");
    ASSERT_NE(k, nullptr);

    auto tm = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext ctx;
    llvm::Module m("xpu_int_sample_reject", ctx);
    configureDeviceModule(m, *tm);
    // The .sample() call on an integer texture is rejected during kernel lowering.
    EXPECT_ANY_THROW(lowerKernel(k, m));
}

// Writable images: Image2D.store(x, y, value) binds a STORAGE_IMAGE descriptor
// and lowers to a single OpImageWrite (the cajeta-spirv llvm.spv.resource.store.2d
// intrinsic). The image declares the R32f known format (matching the runtime
// VK_FORMAT_R32_SFLOAT), so the write needs only the Shader capability — no
// StorageImageWriteWithoutFormat (unsatisfiable for the vulkan1.3-compute triple).
// GPU-free proof that the op is structurally spirv-val-clean under strict
// spirv-val --target-env vulkan1.3.
TEST(XpuVulkanEmitTests, lowersImageStoreToSpirv) {
    auto src =
        "package test;\n"
        "import cajeta.xpu.KernelThread;\n"
        "import cajeta.xpu.Image2D;\n"
        "public class M {\n"
        "    @Kernel\n"
        "    public static void writeImg(Image2D img, uint32 w, uint32 h) {\n"
        "        uint32 i = KernelThread.globalIdX();\n"
        "        if (i < w * h) {\n"
        "            img.store(i % w, i / w, (float32) i);\n"
        "        }\n"
        "    }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.M");
    auto k = findMethod(module->getStructures()["test.M"], "writeImg");
    ASSERT_NE(k, nullptr);

    auto tmIr = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tmIr, nullptr);
    llvm::LLVMContext irCtx;
    llvm::Module irModule("xpu_imgstore_vulkan_ir", irCtx);
    configureDeviceModule(irModule, *tmIr);
    llvm::Function* fn = lowerKernel(k, irModule);
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn->arg_size(), 0u);   // args arrive via descriptors

    std::string ir = printModule(irModule);
    // Storage image bound as a descriptor; written via the store.2d intrinsic.
    EXPECT_NE(ir.find("llvm.spv.resource.handlefrombinding"),
              std::string::npos) << ir;
    EXPECT_NE(ir.find("llvm.spv.resource.store.2d"), std::string::npos) << ir;

    auto tmText = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tmText, nullptr);
    llvm::LLVMContext textCtx;
    llvm::Module textModule("xpu_imgstore_vulkan_text", textCtx);
    configureDeviceModule(textModule, *tmText);
    lowerKernel(k, textModule);
    std::string text = emitSpirvText(textModule, *tmText);
    ASSERT_FALSE(text.empty());
    EXPECT_NE(text.find("OpEntryPoint GLCompute"), std::string::npos) << text;
    EXPECT_NE(text.find("OpTypeImage"), std::string::npos) << text;
    EXPECT_NE(text.find("OpImageWrite"), std::string::npos) << text;
    // R32f known format (matches the runtime VK_FORMAT_R32_SFLOAT) — avoids the
    // StorageImageWriteWithoutFormat capability, which is unsatisfiable for the
    // vulkan1.3-compute triple (it needs SPIR-V >= 1.6).
    EXPECT_NE(text.find("R32f"), std::string::npos) << text;

    auto tmBin = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tmBin, nullptr);
    llvm::LLVMContext binCtx;
    llvm::Module binModule("xpu_imgstore_vulkan_bin", binCtx);
    configureDeviceModule(binModule, *tmBin);
    lowerKernel(k, binModule);
    std::vector<uint8_t> spirv = emitSpirv(binModule, *tmBin);
    ASSERT_FALSE(spirv.empty());
    if (auto valid = validateSpirv(spirv)) {
        EXPECT_TRUE(*valid) << "spirv-val rejected the image-store module";
    } else {
        GTEST_SUCCEED() << "spirv-val not installed; skipped binary validation";
    }
}

// Image read (the read twin of imageStore): Image2D.load(x, y) lowers to a single
// OpImageRead (the cajeta-spirv llvm.spv.resource.load.2d intrinsic) against the
// same R32f STORAGE_IMAGE descriptor. The scalar result is read as a <4 x f32>
// texel with component 0 extracted. GPU-free proof it is spirv-val-clean under
// strict spirv-val --target-env vulkan1.3.
TEST(XpuVulkanEmitTests, lowersImageLoadToSpirv) {
    auto src =
        "package test;\n"
        "import cajeta.xpu.KernelThread;\n"
        "import cajeta.xpu.Image2D;\n"
        "public class M {\n"
        "    @Kernel\n"
        "    public static void rmwImg(Image2D img, uint32 w, uint32 h) {\n"
        "        uint32 i = KernelThread.globalIdX();\n"
        "        if (i < w * h) {\n"
        "            float32 v = img.load(i % w, i / w);\n"
        "            img.store(i % w, i / w, v + 1.0f);\n"
        "        }\n"
        "    }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.M");
    auto k = findMethod(module->getStructures()["test.M"], "rmwImg");
    ASSERT_NE(k, nullptr);

    auto tmIr = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tmIr, nullptr);
    llvm::LLVMContext irCtx;
    llvm::Module irModule("xpu_imgload_vulkan_ir", irCtx);
    configureDeviceModule(irModule, *tmIr);
    llvm::Function* fn = lowerKernel(k, irModule);
    ASSERT_NE(fn, nullptr);

    std::string ir = printModule(irModule);
    EXPECT_NE(ir.find("llvm.spv.resource.load.2d"), std::string::npos) << ir;
    EXPECT_NE(ir.find("llvm.spv.resource.store.2d"), std::string::npos) << ir;

    auto tmText = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tmText, nullptr);
    llvm::LLVMContext textCtx;
    llvm::Module textModule("xpu_imgload_vulkan_text", textCtx);
    configureDeviceModule(textModule, *tmText);
    lowerKernel(k, textModule);
    std::string text = emitSpirvText(textModule, *tmText);
    ASSERT_FALSE(text.empty());
    EXPECT_NE(text.find("OpImageRead"), std::string::npos) << text;
    EXPECT_NE(text.find("OpImageWrite"), std::string::npos) << text;

    auto tmBin = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tmBin, nullptr);
    llvm::LLVMContext binCtx;
    llvm::Module binModule("xpu_imgload_vulkan_bin", binCtx);
    configureDeviceModule(binModule, *tmBin);
    lowerKernel(k, binModule);
    std::vector<uint8_t> spirv = emitSpirv(binModule, *tmBin);
    ASSERT_FALSE(spirv.empty());
    if (auto valid = validateSpirv(spirv)) {
        EXPECT_TRUE(*valid) << "spirv-val rejected the image-load module";
    } else {
        GTEST_SUCCEED() << "spirv-val not installed; skipped binary validation";
    }
}

// cajeta-gpu Part C increment 3a: a kernel that traces a ray query against a
// descriptor-bound AccelerationStructure lowers to the SPV_KHR_ray_query ops.
// The AS binds an ACCELERATION_STRUCTURE_KHR descriptor (handlefrombinding — the
// same path buffers/textures use, proven spirv-val-clean for the AS type in
// increment 2c); the RayQuery local is an OpVariable Function; initialize/
// proceed/committedType/candidateType lower to the llvm.spv.ray.query.*
// intrinsics → OpRayQueryInitializeKHR/ProceedKHR/GetIntersectionTypeKHR. Proven
// against strict spirv-val --target-env vulkan1.3.
TEST(XpuVulkanEmitTests, lowersRayQueryToSpirv) {
    auto src =
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.KernelThread;\n"
        "import cajeta.xpu.AccelerationStructure;\n"
        "import cajeta.xpu.RayQuery;\n"
        "public class M {\n"
        "    @Kernel\n"
        "    public static void trace(AccelerationStructure scene,\n"
        "                             KernelBuffer<uint32> out, uint32 n) {\n"
        "        uint32 i = KernelThread.globalIdX();\n"
        "        if (i < n) {\n"
        "            RayQuery rq;\n"
        "            rq.initialize(scene, 0, 255,\n"
        "                          0.0, 0.0, 0.0, 0.0,\n"
        "                          0.0, 0.0, 1.0, 1000.0);\n"
        "            uint32 acc = 0;\n"
        "            while (rq.proceed()) {\n"
        "                acc = acc + rq.candidateType();\n"
        "            }\n"
        "            out[i] = acc + rq.committedType();\n"
        "        }\n"
        "    }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.M");
    auto k = findMethod(module->getStructures()["test.M"], "trace");
    ASSERT_NE(k, nullptr);

    auto tmIr = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tmIr, nullptr);
    llvm::LLVMContext irCtx;
    llvm::Module irModule("xpu_rayquery_vulkan_ir", irCtx);
    configureDeviceModule(irModule, *tmIr);
    llvm::Function* fn = lowerKernel(k, irModule);
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn->arg_size(), 0u);   // args arrive via descriptors

    std::string ir = printModule(irModule);
    // AS bound as a descriptor; the three ray-query intrinsics emitted.
    EXPECT_NE(ir.find("llvm.spv.resource.handlefrombinding"),
              std::string::npos) << ir;
    EXPECT_NE(ir.find("llvm.spv.ray.query.initialize"), std::string::npos) << ir;
    EXPECT_NE(ir.find("llvm.spv.ray.query.proceed"), std::string::npos) << ir;
    EXPECT_NE(ir.find("llvm.spv.ray.query.get.intersection.type"),
              std::string::npos) << ir;

    auto tmText = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tmText, nullptr);
    llvm::LLVMContext textCtx;
    llvm::Module textModule("xpu_rayquery_vulkan_text", textCtx);
    configureDeviceModule(textModule, *tmText);
    lowerKernel(k, textModule);
    std::string text = emitSpirvText(textModule, *tmText);
    ASSERT_FALSE(text.empty());
    EXPECT_NE(text.find("OpEntryPoint GLCompute"), std::string::npos) << text;
    EXPECT_NE(text.find("OpCapability RayQueryKHR"), std::string::npos) << text;
    EXPECT_NE(text.find("OpExtension \"SPV_KHR_ray_query\""),
              std::string::npos) << text;
    EXPECT_NE(text.find("OpTypeAccelerationStructureKHR"),
              std::string::npos) << text;
    EXPECT_NE(text.find("OpTypeRayQueryKHR"), std::string::npos) << text;
    EXPECT_NE(text.find("OpRayQueryInitializeKHR"), std::string::npos) << text;
    EXPECT_NE(text.find("OpRayQueryProceedKHR"), std::string::npos) << text;
    EXPECT_NE(text.find("OpRayQueryGetIntersectionTypeKHR"),
              std::string::npos) << text;

    auto tmBin = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tmBin, nullptr);
    llvm::LLVMContext binCtx;
    llvm::Module binModule("xpu_rayquery_vulkan_bin", binCtx);
    configureDeviceModule(binModule, *tmBin);
    lowerKernel(k, binModule);
    std::vector<uint8_t> spirv = emitSpirv(binModule, *tmBin);
    ASSERT_FALSE(spirv.empty());
    if (auto valid = validateSpirv(spirv)) {
        EXPECT_TRUE(*valid) << "spirv-val rejected the ray-query module";
    } else {
        GTEST_SUCCEED() << "spirv-val not installed; skipped binary validation";
    }
}

// candidatePrimitiveIndex() (Toffee P1.1 exact-L2 refinement) lowers to
// llvm.spv.ray.query.get.intersection.primitive.index ->
// OpRayQueryGetIntersectionPrimitiveIndexKHR (opcode 4487, intersection=Candidate).
// This is a GPU-FREE check that the new op is structurally spirv-val-clean,
// independent of any one driver's shader compiler accepting it at pipeline build.
TEST(XpuVulkanEmitTests, lowersRayQueryPrimitiveIndexToSpirv) {
    auto src =
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.KernelThread;\n"
        "import cajeta.xpu.AccelerationStructure;\n"
        "import cajeta.xpu.RayQuery;\n"
        "public class M {\n"
        "    @Kernel\n"
        "    public static void trace(AccelerationStructure scene,\n"
        "                             KernelBuffer<float32> dp,\n"
        "                             KernelBuffer<uint32> out, uint32 n) {\n"
        "        uint32 i = KernelThread.globalIdX();\n"
        "        if (i < n) {\n"
        "            RayQuery rq;\n"
        "            rq.initialize(scene, 0, 255,\n"
        "                          0.0f, 0.0f, 0.0f, 0.0f,\n"
        "                          0.0f, 0.0f, 1.0f, 1000.0f);\n"
        "            float32 acc = 0.0f;\n"
        "            while (rq.proceed()) {\n"
        "                if (rq.candidateType() == 1) {\n"
        "                    uint32 pi = rq.candidatePrimitiveIndex();\n"
        "                    acc = acc + dp[pi];\n"
        "                }\n"
        "            }\n"
        "            out[i] = (uint32) acc;\n"
        "        }\n"
        "    }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.M");
    auto k = findMethod(module->getStructures()["test.M"], "trace");
    ASSERT_NE(k, nullptr);

    auto tmIr = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tmIr, nullptr);
    llvm::LLVMContext irCtx;
    llvm::Module irModule("xpu_rq_primidx_ir", irCtx);
    configureDeviceModule(irModule, *tmIr);
    ASSERT_NE(lowerKernel(k, irModule), nullptr);
    std::string ir = printModule(irModule);
    EXPECT_NE(ir.find("llvm.spv.ray.query.get.intersection.primitive.index"),
              std::string::npos) << ir;

    auto tmText = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tmText, nullptr);
    llvm::LLVMContext textCtx;
    llvm::Module textModule("xpu_rq_primidx_text", textCtx);
    configureDeviceModule(textModule, *tmText);
    lowerKernel(k, textModule);
    std::string text = emitSpirvText(textModule, *tmText);
    ASSERT_FALSE(text.empty());
    EXPECT_NE(text.find("OpRayQueryGetIntersectionPrimitiveIndexKHR"),
              std::string::npos) << text;

    auto tmBin = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tmBin, nullptr);
    llvm::LLVMContext binCtx;
    llvm::Module binModule("xpu_rq_primidx_bin", binCtx);
    configureDeviceModule(binModule, *tmBin);
    lowerKernel(k, binModule);
    std::vector<uint8_t> spirv = emitSpirv(binModule, *tmBin);
    ASSERT_FALSE(spirv.empty());
    if (auto valid = validateSpirv(spirv)) {
        EXPECT_TRUE(*valid) << "spirv-val rejected the primitive-index module";
    } else {
        GTEST_SUCCEED() << "spirv-val not installed; skipped binary validation";
    }
}

// CooperativeMatrix<T,Rows,Cols,Use> (cajeta-gpu CM4): the cajeta device surface
// for SPV_KHR_cooperative_matrix. A @Kernel doing one 16x16 f32 matmul tile —
// load A (use 0) + B (use 1), splat-zero accumulator C (use 2), mma, store C —
// lowers to the llvm.spv.cooperative.matrix.* intrinsics and emits a spirv-val-
// clean module (OpTypeCooperativeMatrixKHR + the ops + OpMemoryModel Logical
// VulkanKHR). GPU-free: this proves the surface + lowering, independent of any
// driver's cooperative-matrix config support (which CM5 exec-checks on device).
TEST(XpuVulkanEmitTests, lowersCooperativeMatrixToSpirv) {
    auto src =
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.CooperativeMatrix;\n"
        "public class M {\n"
        "    @Kernel\n"
        "    public static void matmul(KernelBuffer<float32> a, KernelBuffer<float32> b,\n"
        "                              KernelBuffer<float32> c) {\n"
        "        CooperativeMatrix<float32,16,16,0> ma;\n"
        "        ma.load(a, 0, 0, 16);\n"
        "        CooperativeMatrix<float32,16,16,1> mb;\n"
        "        mb.load(b, 0, 0, 16);\n"
        "        CooperativeMatrix<float32,16,16,2> mc;\n"
        "        mc.splat(0.0f);\n"
        "        mc.mma(ma, mb);\n"
        "        mc.store(c, 0, 0, 16);\n"
        "    }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.M");
    auto k = findMethod(module->getStructures()["test.M"], "matmul");
    ASSERT_NE(k, nullptr);

    auto tmIr = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tmIr, nullptr);
    llvm::LLVMContext irCtx;
    llvm::Module irModule("xpu_coopmat_ir", irCtx);
    configureDeviceModule(irModule, *tmIr);
    ASSERT_NE(lowerKernel(k, irModule), nullptr);
    std::string ir = printModule(irModule);
    EXPECT_NE(ir.find("llvm.spv.cooperative.matrix.load"), std::string::npos) << ir;
    EXPECT_NE(ir.find("llvm.spv.cooperative.matrix.muladd"), std::string::npos) << ir;
    EXPECT_NE(ir.find("llvm.spv.cooperative.matrix.store"), std::string::npos) << ir;

    auto tmText = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tmText, nullptr);
    llvm::LLVMContext textCtx;
    llvm::Module textModule("xpu_coopmat_text", textCtx);
    configureDeviceModule(textModule, *tmText);
    lowerKernel(k, textModule);
    std::string text = emitSpirvText(textModule, *tmText);
    ASSERT_FALSE(text.empty());
    EXPECT_NE(text.find("OpCapability CooperativeMatrixKHR"), std::string::npos) << text;
    EXPECT_NE(text.find("OpMemoryModel Logical Vulkan"), std::string::npos) << text;
    EXPECT_NE(text.find("OpTypeCooperativeMatrixKHR"), std::string::npos) << text;
    EXPECT_NE(text.find("OpCooperativeMatrixLoadKHR"), std::string::npos) << text;
    EXPECT_NE(text.find("OpCooperativeMatrixMulAddKHR"), std::string::npos) << text;
    EXPECT_NE(text.find("OpCooperativeMatrixStoreKHR"), std::string::npos) << text;

    auto tmBin = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tmBin, nullptr);
    llvm::LLVMContext binCtx;
    llvm::Module binModule("xpu_coopmat_bin", binCtx);
    configureDeviceModule(binModule, *tmBin);
    lowerKernel(k, binModule);
    std::vector<uint8_t> spirv = emitSpirv(binModule, *tmBin);
    ASSERT_FALSE(spirv.empty());
    if (auto valid = validateSpirv(spirv)) {
        EXPECT_TRUE(*valid) << "spirv-val rejected the cooperative-matrix module";
    } else {
        GTEST_SUCCEED() << "spirv-val not installed; skipped binary validation";
    }
}

// Integer dot product (SPV_KHR_integer_dot_product, DP4a). `Vector<int8,4>` /
// `Vector<uint8,4>` `.dot()` lowers to llvm.spv.dot4add.{i8,u8}packed, which the
// SPIR-V backend selects to OpSDot / OpUDot ... PackedVectorFormat4x8Bit + OpIAdd
// (with the DotProduct / DotProductInput4x8BitPacked capabilities + the
// SPV_KHR_integer_dot_product extension). Signedness comes from the element type;
// the optional second arg is the int32 accumulator (fused dot-add). GPU-free:
// proves the lowering and that the module is spirv-val-clean under Vulkan 1.3.
TEST(XpuVulkanEmitTests, lowersIntegerDotToSpirv) {
    auto src =
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "public class D {\n"
        "    @Kernel\n"
        "    public static void dp4a(KernelBuffer<int32> out, uint32 n) {\n"
        "        Vector<int8,4> a = heap Vector<int8,4>(1, 2, 3, 4);\n"
        "        Vector<int8,4> b = heap Vector<int8,4>(5, 6, 7, 8);\n"
        "        int32 d = a.dot(b);\n"          // signed dot
        "        int32 e = a.dot(b, d);\n"       // fused signed dot-add
        "        Vector<uint8,4> u = heap Vector<uint8,4>(9, 10, 11, 12);\n"
        "        int32 f = u.dot(u);\n"          // unsigned dot
        "        out[0] = e + f;\n"
        "    }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.D");
    auto k = findMethod(module->getStructures()["test.D"], "dp4a");
    ASSERT_NE(k, nullptr);

    auto tmIr = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tmIr, nullptr);
    llvm::LLVMContext irCtx;
    llvm::Module irModule("xpu_idot_ir", irCtx);
    configureDeviceModule(irModule, *tmIr);
    ASSERT_NE(lowerKernel(k, irModule), nullptr);
    std::string ir = printModule(irModule);
    EXPECT_NE(ir.find("llvm.spv.dot4add.i8packed"), std::string::npos) << ir;
    EXPECT_NE(ir.find("llvm.spv.dot4add.u8packed"), std::string::npos) << ir;

    auto tmText = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tmText, nullptr);
    llvm::LLVMContext textCtx;
    llvm::Module textModule("xpu_idot_text", textCtx);
    configureDeviceModule(textModule, *tmText);
    lowerKernel(k, textModule);
    std::string text = emitSpirvText(textModule, *tmText);
    ASSERT_FALSE(text.empty());
    EXPECT_NE(text.find("OpCapability DotProduct"), std::string::npos) << text;
    EXPECT_NE(text.find("OpCapability DotProductInput4x8BitPacked"),
              std::string::npos) << text;
    EXPECT_NE(text.find("OpExtension \"SPV_KHR_integer_dot_product\""),
              std::string::npos) << text;
    EXPECT_NE(text.find("OpSDot"), std::string::npos) << text;
    EXPECT_NE(text.find("OpUDot"), std::string::npos) << text;
    EXPECT_NE(text.find("PackedVectorFormat4x8Bit"), std::string::npos) << text;

    auto tmBin = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tmBin, nullptr);
    llvm::LLVMContext binCtx;
    llvm::Module binModule("xpu_idot_bin", binCtx);
    configureDeviceModule(binModule, *tmBin);
    lowerKernel(k, binModule);
    std::vector<uint8_t> spirv = emitSpirv(binModule, *tmBin);
    ASSERT_FALSE(spirv.empty());
    if (auto valid = validateSpirv(spirv)) {
        EXPECT_TRUE(*valid) << "spirv-val rejected the integer-dot module";
    } else {
        GTEST_SUCCEED() << "spirv-val not installed; skipped binary validation";
    }
}

// Float atomics (SPV_EXT_shader_atomic_float_add / _min_max). `KernelBuffer<float32>`
// `.atomicAdd/atomicMin/atomicMax(i, v)` lower to atomicrmw fadd/fmin/fmax with
// Device scope + AcquireRelease (the Vulkan-valid combination), which the SPIR-V
// backend selects to OpAtomicFAddEXT / FMinEXT / FMaxEXT under the
// AtomicFloat32AddEXT / AtomicFloat32MinMaxEXT capabilities. GPU-free: proves the
// lowering and that the module is spirv-val-clean under Vulkan 1.3.
TEST(XpuVulkanEmitTests, lowersFloatAtomicsToSpirv) {
    auto src =
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.KernelThread;\n"
        "public class A {\n"
        "    @Kernel\n"
        "    public static void reduce(KernelBuffer<float32> acc, uint32 n) {\n"
        "        uint32 i = KernelThread.globalIdX();\n"
        "        if (i < n) {\n"
        "            acc.atomicAdd(0, 1.0f);\n"
        "            acc.atomicMin(1, (float32) i);\n"
        "            acc.atomicMax(2, (float32) i);\n"
        "        }\n"
        "    }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.A");
    auto k = findMethod(module->getStructures()["test.A"], "reduce");
    ASSERT_NE(k, nullptr);

    auto tmIr = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tmIr, nullptr);
    llvm::LLVMContext irCtx;
    llvm::Module irModule("xpu_fatomic_ir", irCtx);
    configureDeviceModule(irModule, *tmIr);
    ASSERT_NE(lowerKernel(k, irModule), nullptr);
    std::string ir = printModule(irModule);
    EXPECT_NE(ir.find("atomicrmw fadd"), std::string::npos) << ir;
    EXPECT_NE(ir.find("atomicrmw fmin"), std::string::npos) << ir;
    EXPECT_NE(ir.find("atomicrmw fmax"), std::string::npos) << ir;
    EXPECT_NE(ir.find("syncscope(\"device\") acq_rel"), std::string::npos) << ir;

    auto tmText = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tmText, nullptr);
    llvm::LLVMContext textCtx;
    llvm::Module textModule("xpu_fatomic_text", textCtx);
    configureDeviceModule(textModule, *tmText);
    lowerKernel(k, textModule);
    std::string text = emitSpirvText(textModule, *tmText);
    ASSERT_FALSE(text.empty());
    EXPECT_NE(text.find("OpCapability AtomicFloat32AddEXT"), std::string::npos) << text;
    EXPECT_NE(text.find("OpCapability AtomicFloat32MinMaxEXT"), std::string::npos) << text;
    EXPECT_NE(text.find("OpExtension \"SPV_EXT_shader_atomic_float_add\""),
              std::string::npos) << text;
    EXPECT_NE(text.find("OpExtension \"SPV_EXT_shader_atomic_float_min_max\""),
              std::string::npos) << text;
    EXPECT_NE(text.find("OpAtomicFAddEXT"), std::string::npos) << text;
    EXPECT_NE(text.find("OpAtomicFMinEXT"), std::string::npos) << text;
    EXPECT_NE(text.find("OpAtomicFMaxEXT"), std::string::npos) << text;

    auto tmBin = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tmBin, nullptr);
    llvm::LLVMContext binCtx;
    llvm::Module binModule("xpu_fatomic_bin", binCtx);
    configureDeviceModule(binModule, *tmBin);
    lowerKernel(k, binModule);
    std::vector<uint8_t> spirv = emitSpirv(binModule, *tmBin);
    ASSERT_FALSE(spirv.empty());
    if (auto valid = validateSpirv(spirv)) {
        EXPECT_TRUE(*valid) << "spirv-val rejected the float-atomics module";
    } else {
        GTEST_SUCCEED() << "spirv-val not installed; skipped binary validation";
    }
}

// Integer atomics: KernelBuffer<int32> atomic{Add,Sub,Min,Max,And,Or,Xor,Exchange} and
// atomicCompareExchange lower to core OpAtomicI*/CompareExchange (NO extension —
// unlike the float path). Signed buffer -> OpAtomicSMin/SMax. Device scope +
// AcquireRelease (same memory-model constraint as floats). GPU-free; spirv-val
// clean under Vulkan 1.3.
TEST(XpuVulkanEmitTests, lowersIntAtomicsToSpirv) {
    auto src =
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.KernelThread;\n"
        "public class A {\n"
        "    @Kernel\n"
        "    public static void atomics(KernelBuffer<int32> a, uint32 n) {\n"
        "        uint32 i = KernelThread.globalIdX();\n"
        "        if (i < n) {\n"
        "            a.atomicAdd(0, 1);\n"
        "            a.atomicSub(1, 1);\n"
        "            a.atomicMin(2, (int32) i);\n"
        "            a.atomicMax(3, (int32) i);\n"
        "            a.atomicAnd(4, (int32) i);\n"
        "            a.atomicOr(5, (int32) i);\n"
        "            a.atomicXor(6, (int32) i);\n"
        "            a.atomicExchange(7, (int32) i);\n"
        "            a.atomicCompareExchange(8, 0, (int32) i);\n"
        "        }\n"
        "    }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.A");
    auto k = findMethod(module->getStructures()["test.A"], "atomics");
    ASSERT_NE(k, nullptr);

    auto tmIr = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tmIr, nullptr);
    llvm::LLVMContext irCtx;
    llvm::Module irModule("xpu_iatomic_ir", irCtx);
    configureDeviceModule(irModule, *tmIr);
    ASSERT_NE(lowerKernel(k, irModule), nullptr);
    std::string ir = printModule(irModule);
    EXPECT_NE(ir.find("atomicrmw add"), std::string::npos) << ir;
    EXPECT_NE(ir.find("atomicrmw sub"), std::string::npos) << ir;
    EXPECT_NE(ir.find("atomicrmw min"), std::string::npos) << ir;   // signed
    EXPECT_NE(ir.find("atomicrmw max"), std::string::npos) << ir;
    EXPECT_NE(ir.find("atomicrmw xchg"), std::string::npos) << ir;
    EXPECT_NE(ir.find("cmpxchg"), std::string::npos) << ir;
    EXPECT_NE(ir.find("syncscope(\"device\") acq_rel"), std::string::npos) << ir;

    auto tmText = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tmText, nullptr);
    llvm::LLVMContext textCtx;
    llvm::Module textModule("xpu_iatomic_text", textCtx);
    configureDeviceModule(textModule, *tmText);
    lowerKernel(k, textModule);
    std::string text = emitSpirvText(textModule, *tmText);
    ASSERT_FALSE(text.empty());
    EXPECT_NE(text.find("OpAtomicIAdd"), std::string::npos) << text;
    EXPECT_NE(text.find("OpAtomicISub"), std::string::npos) << text;
    EXPECT_NE(text.find("OpAtomicSMin"), std::string::npos) << text;
    EXPECT_NE(text.find("OpAtomicSMax"), std::string::npos) << text;
    EXPECT_NE(text.find("OpAtomicAnd"), std::string::npos) << text;
    EXPECT_NE(text.find("OpAtomicOr"), std::string::npos) << text;
    EXPECT_NE(text.find("OpAtomicXor"), std::string::npos) << text;
    EXPECT_NE(text.find("OpAtomicExchange"), std::string::npos) << text;

    auto tmBin = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tmBin, nullptr);
    llvm::LLVMContext binCtx;
    llvm::Module binModule("xpu_iatomic_bin", binCtx);
    configureDeviceModule(binModule, *tmBin);
    lowerKernel(k, binModule);
    std::vector<uint8_t> spirv = emitSpirv(binModule, *tmBin);
    ASSERT_FALSE(spirv.empty());
    if (auto valid = validateSpirv(spirv)) {
        EXPECT_TRUE(*valid) << "spirv-val rejected the int-atomics module";
    } else {
        GTEST_SUCCEED() << "spirv-val not installed; skipped binary validation";
    }
}

// Shared-memory atomics: an atomic on `shared T[]` (Workgroup storage) must carry
// WORKGROUP scope, while an atomic on a global KernelBuffer carries DEVICE scope — the
// scope is picked from the pointer's address space. This kernel does both, so the
// IR must contain syncscope("workgroup") AND syncscope("device"); spirv-val clean.
TEST(XpuVulkanEmitTests, lowersSharedAtomicsToSpirv) {
    auto src =
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.KernelThread;\n"
        "import cajeta.xpu.Barrier;\n"
        "import cajeta.xpu.Shared;\n"
        "public class A {\n"
        "    @Kernel\n"
        "    public static void mixed(KernelBuffer<uint32> out, uint32 n) {\n"
        "        Shared<uint32> acc = shared uint32[64];\n"
        "        uint32 t = KernelThread.x();\n"
        "        if (t == 0) { acc[0] = 0; }\n"
        "        Barrier.workgroup();\n"
        "        acc.atomicAdd(0, 1);\n"   // shared -> Workgroup scope
        "        out.atomicAdd(0, 1);\n"   // global -> Device scope
        "    }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.A");
    auto k = findMethod(module->getStructures()["test.A"], "mixed");
    ASSERT_NE(k, nullptr);

    auto tmIr = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tmIr, nullptr);
    llvm::LLVMContext irCtx;
    llvm::Module irModule("xpu_shatomic_ir", irCtx);
    configureDeviceModule(irModule, *tmIr);
    ASSERT_NE(lowerKernel(k, irModule), nullptr);
    std::string ir = printModule(irModule);
    // Both scopes present — shared atomic at Workgroup, global atomic at Device.
    EXPECT_NE(ir.find("syncscope(\"workgroup\")"), std::string::npos) << ir;
    EXPECT_NE(ir.find("syncscope(\"device\")"), std::string::npos) << ir;

    auto tmBin = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tmBin, nullptr);
    llvm::LLVMContext binCtx;
    llvm::Module binModule("xpu_shatomic_bin", binCtx);
    configureDeviceModule(binModule, *tmBin);
    lowerKernel(k, binModule);
    std::vector<uint8_t> spirv = emitSpirv(binModule, *tmBin);
    ASSERT_FALSE(spirv.empty());
    if (auto valid = validateSpirv(spirv)) {
        EXPECT_TRUE(*valid) << "spirv-val rejected the shared-atomics module";
    } else {
        GTEST_SUCCEED() << "spirv-val not installed; skipped binary validation";
    }
}

// Shader clock (SPV_KHR_shader_clock). `KernelThread.clock()` lowers to the fork's
// llvm.spv.read.clock intrinsic, which the SPIR-V backend selects to
// OpReadClockKHR at Subgroup scope under the ShaderClockKHR capability. GPU-free:
// proves the lowering and that the module is spirv-val-clean under Vulkan 1.3.
TEST(XpuVulkanEmitTests, lowersShaderClockToSpirv) {
    auto src =
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.KernelThread;\n"
        "public class C {\n"
        "    @Kernel\n"
        "    public static void stamp(KernelBuffer<uint64> out, uint32 n) {\n"
        "        uint32 i = KernelThread.globalIdX();\n"
        "        if (i < n) {\n"
        "            out[i] = KernelThread.clock();\n"
        "        }\n"
        "    }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.C");
    auto k = findMethod(module->getStructures()["test.C"], "stamp");
    ASSERT_NE(k, nullptr);

    auto tmIr = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tmIr, nullptr);
    llvm::LLVMContext irCtx;
    llvm::Module irModule("xpu_clock_ir", irCtx);
    configureDeviceModule(irModule, *tmIr);
    ASSERT_NE(lowerKernel(k, irModule), nullptr);
    std::string ir = printModule(irModule);
    EXPECT_NE(ir.find("llvm.spv.read.clock"), std::string::npos) << ir;

    auto tmText = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tmText, nullptr);
    llvm::LLVMContext textCtx;
    llvm::Module textModule("xpu_clock_text", textCtx);
    configureDeviceModule(textModule, *tmText);
    lowerKernel(k, textModule);
    std::string text = emitSpirvText(textModule, *tmText);
    ASSERT_FALSE(text.empty());
    EXPECT_NE(text.find("OpCapability ShaderClockKHR"), std::string::npos) << text;
    EXPECT_NE(text.find("OpExtension \"SPV_KHR_shader_clock\""),
              std::string::npos) << text;
    EXPECT_NE(text.find("OpReadClockKHR"), std::string::npos) << text;

    auto tmBin = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tmBin, nullptr);
    llvm::LLVMContext binCtx;
    llvm::Module binModule("xpu_clock_bin", binCtx);
    configureDeviceModule(binModule, *tmBin);
    lowerKernel(k, binModule);
    std::vector<uint8_t> spirv = emitSpirv(binModule, *tmBin);
    ASSERT_FALSE(spirv.empty());
    if (auto valid = validateSpirv(spirv)) {
        EXPECT_TRUE(*valid) << "spirv-val rejected the shader-clock module";
    } else {
        GTEST_SUCCEED() << "spirv-val not installed; skipped binary validation";
    }
}

// Bit instructions (no extension, no fork). Bits.reverse/count/rotateLeft/
// rotateRight lower to the GENERIC llvm.bitreverse / ctpop / fshl / fshr
// intrinsics, which the SPIR-V backend already selects to core OpBitReverse /
// OpBitCount (+ funnel-shift expansion) under the plain Shader capability — they
// reach the Vulkan/Shader flavor with no llvm.spv.* intrinsic. GPU-free: proves
// the lowering and that the module is spirv-val-clean under Vulkan 1.3.
TEST(XpuVulkanEmitTests, lowersBitInstructionsToSpirv) {
    auto src =
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.KernelThread;\n"
        "import cajeta.xpu.Bits;\n"
        "public class B {\n"
        "    @Kernel\n"
        "    public static void bitops(KernelBuffer<uint32> out, uint32 n) {\n"
        "        uint32 i = KernelThread.globalIdX();\n"
        "        if (i < n) {\n"
        "            uint32 v = i;\n"
        "            v = Bits.reverse(v);\n"
        "            v = v + Bits.count(i);\n"
        "            v = v + Bits.rotateLeft(i, 3);\n"
        "            v = v + Bits.rotateRight(i, 5);\n"
        "            out[i] = v;\n"
        "        }\n"
        "    }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.B");
    auto k = findMethod(module->getStructures()["test.B"], "bitops");
    ASSERT_NE(k, nullptr);

    auto tmIr = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tmIr, nullptr);
    llvm::LLVMContext irCtx;
    llvm::Module irModule("xpu_bits_ir", irCtx);
    configureDeviceModule(irModule, *tmIr);
    ASSERT_NE(lowerKernel(k, irModule), nullptr);
    std::string ir = printModule(irModule);
    EXPECT_NE(ir.find("llvm.bitreverse.i32"), std::string::npos) << ir;
    EXPECT_NE(ir.find("llvm.ctpop.i32"), std::string::npos) << ir;
    // Rotates are expanded inline (shl/lshr/or), NOT llvm.fshl/fshr — the
    // funnel-shift intrinsics lower to an external-linkage helper that pulls in
    // OpCapability Linkage (invalid under Vulkan). Assert they are absent.
    EXPECT_EQ(ir.find("llvm.fshl"), std::string::npos) << ir;
    EXPECT_EQ(ir.find("llvm.fshr"), std::string::npos) << ir;

    auto tmText = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tmText, nullptr);
    llvm::LLVMContext textCtx;
    llvm::Module textModule("xpu_bits_text", textCtx);
    configureDeviceModule(textModule, *tmText);
    lowerKernel(k, textModule);
    std::string text = emitSpirvText(textModule, *tmText);
    ASSERT_FALSE(text.empty());
    EXPECT_NE(text.find("OpBitReverse"), std::string::npos) << text;
    EXPECT_NE(text.find("OpBitCount"), std::string::npos) << text;

    auto tmBin = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tmBin, nullptr);
    llvm::LLVMContext binCtx;
    llvm::Module binModule("xpu_bits_bin", binCtx);
    configureDeviceModule(binModule, *tmBin);
    lowerKernel(k, binModule);
    std::vector<uint8_t> spirv = emitSpirv(binModule, *tmBin);
    ASSERT_FALSE(spirv.empty());
    if (auto valid = validateSpirv(spirv)) {
        EXPECT_TRUE(*valid) << "spirv-val rejected the bit-instructions module";
    } else {
        GTEST_SUCCEED() << "spirv-val not installed; skipped binary validation";
    }
}

// CM5b — the device-realistic MIXED-PRECISION config: A and B are float16
// (IEEE half) tiles, the accumulator C is float32. This is the only float
// cooperative-matrix config the RADV STRIX_HALO WMMA path actually supports
// (f16/f16 -> f32, M=N=K=16) and the regime SPELA/Toffee matmul runs in. The
// CooperativeMatrix surface needs no change: the device lowerer keys each tile
// off its own declared element type, and every seam (load/muladd/store) is
// independently overloaded, so a half-input/float-accumulate mma lowers and
// validates. Proves the emit half: OpTypeFloat 16 element (Float16 capability) +
// f16/f16->f32 OpCooperativeMatrixMulAddKHR, spirv-val-clean. CM5c exec-checks
// the same config on the real device.
TEST(XpuVulkanEmitTests, lowersMixedPrecisionCooperativeMatrixToSpirv) {
    auto src =
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.CooperativeMatrix;\n"
        "public class M {\n"
        "    @Kernel\n"
        "    public static void matmul(KernelBuffer<float16> a, KernelBuffer<float16> b,\n"
        "                              KernelBuffer<float32> c) {\n"
        "        CooperativeMatrix<float16,16,16,0> ma;\n"
        "        ma.load(a, 0, 0, 16);\n"
        "        CooperativeMatrix<float16,16,16,1> mb;\n"
        "        mb.load(b, 0, 0, 16);\n"
        "        CooperativeMatrix<float32,16,16,2> mc;\n"
        "        mc.splat(0.0f);\n"
        "        mc.mma(ma, mb);\n"
        "        mc.store(c, 0, 0, 16);\n"
        "    }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.M");
    auto k = findMethod(module->getStructures()["test.M"], "matmul");
    ASSERT_NE(k, nullptr);

    auto tmIr = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tmIr, nullptr);
    llvm::LLVMContext irCtx;
    llvm::Module irModule("xpu_coopmat_mixed_ir", irCtx);
    configureDeviceModule(irModule, *tmIr);
    ASSERT_NE(lowerKernel(k, irModule), nullptr);
    std::string ir = printModule(irModule);
    // A/B loads are overloaded on the half matrix type; the muladd result is the
    // float accumulator — the two element types coexist in one mma.
    EXPECT_NE(ir.find("llvm.spv.cooperative.matrix.load"), std::string::npos) << ir;
    EXPECT_NE(ir.find("llvm.spv.cooperative.matrix.muladd"), std::string::npos) << ir;
    EXPECT_NE(ir.find("half"), std::string::npos) << ir;

    auto tmText = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tmText, nullptr);
    llvm::LLVMContext textCtx;
    llvm::Module textModule("xpu_coopmat_mixed_text", textCtx);
    configureDeviceModule(textModule, *tmText);
    lowerKernel(k, textModule);
    std::string text = emitSpirvText(textModule, *tmText);
    ASSERT_FALSE(text.empty());
    EXPECT_NE(text.find("OpCapability Float16"), std::string::npos) << text;
    EXPECT_NE(text.find("OpCapability CooperativeMatrixKHR"), std::string::npos) << text;
    EXPECT_NE(text.find("OpMemoryModel Logical Vulkan"), std::string::npos) << text;
    EXPECT_NE(text.find("OpTypeFloat 16"), std::string::npos) << text;
    EXPECT_NE(text.find("OpCooperativeMatrixMulAddKHR"), std::string::npos) << text;

    auto tmBin = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tmBin, nullptr);
    llvm::LLVMContext binCtx;
    llvm::Module binModule("xpu_coopmat_mixed_bin", binCtx);
    configureDeviceModule(binModule, *tmBin);
    lowerKernel(k, binModule);
    std::vector<uint8_t> spirv = emitSpirv(binModule, *tmBin);
    ASSERT_FALSE(spirv.empty());
    if (auto valid = validateSpirv(spirv)) {
        EXPECT_TRUE(*valid) << "spirv-val rejected the mixed-precision cooperative-matrix module";
    } else {
        GTEST_SUCCEED() << "spirv-val not installed; skipped binary validation";
    }
}

// A workgroup-shared kernel with a barrier emits a SPIR-V module that is
// strictly Vulkan-VALID — proving the barrier post-pass (SpirvBackend's
// fixupControlBarriers) rewrites LLVM 23's forbidden SequentiallyConsistent
// OpControlBarrier semantics to WorkgroupMemory|AcquireRelease. Without the
// fixup this module fails `spirv-val` (VUID-StandaloneSpirv-MemorySemantics).
TEST(XpuVulkanEmitTests, workgroupBarrierIsSpecValid) {
    auto src =
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.KernelThread;\n"
        "import cajeta.xpu.Barrier;\n"
        "import cajeta.xpu.Shared;\n"
        "public class M {\n"
        "    @Kernel\n"
        "    public static void reduce(KernelBuffer<int32> out, KernelBuffer<int32> in) {\n"
        "        Shared<int32> tile = shared int32[64];\n"
        "        uint32 t = KernelThread.x();\n"
        "        tile[t] = in[t];\n"
        "        Barrier.workgroup();\n"
        "        if (t == 0) { out[0] = tile[0]; }\n"
        "    }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.M");
    auto reduce = findMethod(module->getStructures()["test.M"], "reduce");
    ASSERT_NE(reduce, nullptr);

    auto tm = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext deviceCtx;
    llvm::Module deviceModule("xpu_barrier_vulkan", deviceCtx);
    configureDeviceModule(deviceModule, *tm);
    lowerKernel(reduce, deviceModule);
    std::vector<uint8_t> spirv = emitSpirv(deviceModule, *tm);
    ASSERT_FALSE(spirv.empty());

    { std::ofstream o("/tmp/probe_shared_coop.spv", std::ios::binary); o.write((const char*)spirv.data(), (std::streamsize)spirv.size()); }
    auto valid = validateSpirv(spirv);
    if (!valid) {
        GTEST_SUCCEED() << "spirv-val not installed; barrier validity unchecked";
        return;
    }
    EXPECT_TRUE(*valid) << "barrier kernel failed spirv-val — the OpControlBarrier "
                           "semantics fixup did not take effect";
}

// LDS-staged cooperative matrix on Vulkan: a CooperativeMatrix.load from a
// Shared<T> (Workgroup-storage) tile, fed by a cooperative global->LDS staging
// copy, lowers to VALID Vulkan SPIR-V. Exercises the two fork SPIR-V backend
// fixes: (1) SPIRVEmitIntrinsics keeps the undef non-constant array global
// correctly typed (the staged array indexed by a loop variable keeps its array
// type + OpAccessChain index), and (2) the cooperative-matrix selection
// access-chains the aggregate Workgroup pointer to its first element so
// OpCooperativeMatrixLoadKHR gets the scalar pointer it requires.
TEST(XpuVulkanEmitTests, ldsStagedCoopLoadEmitsValidSpirv) {
    auto src =
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.CooperativeMatrix;\n"
        "import cajeta.xpu.CoopStage;\n"
        "import cajeta.xpu.Barrier;\n"
        "public class M {\n"
        "    @Kernel\n"
        "    public static void staged(KernelBuffer<float16> a, KernelBuffer<float16> b,\n"
        "                              KernelBuffer<float32> c) {\n"
        "        Shared<float16> sa = shared float16[16 * 16];\n"
        "        CoopStage.panel(sa, a, 0, 0, 16, 16, 16);\n"
        "        Barrier.workgroup();\n"
        "        CooperativeMatrix<float16,16,16,0> ma;\n"
        "        ma.load(sa, 0, 0, 16);\n"
        "        CooperativeMatrix<float16,16,16,1> mb;\n"
        "        mb.load(b, 0, 0, 16);\n"
        "        CooperativeMatrix<float32,16,16,2> mc;\n"
        "        mc.splat(0.0f);\n"
        "        mc.mma(ma, mb);\n"
        "        mc.store(c, 0, 0, 16);\n"
        "    }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.M");
    auto k = findMethod(module->getStructures()["test.M"], "staged");
    ASSERT_NE(k, nullptr);

    auto tm = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext ctx;
    llvm::Module mod("xpu_sharedcoop_vulkan", ctx);
    configureDeviceModule(mod, *tm);
    ASSERT_NE(lowerKernel(k, mod), nullptr);
    std::vector<uint8_t> spirv = emitSpirv(mod, *tm);
    ASSERT_FALSE(spirv.empty());
    if (auto valid = validateSpirv(spirv))
        EXPECT_TRUE(*valid) << "LDS-staged cooperative-matrix load produced "
                               "invalid SPIR-V";
    else
        GTEST_SUCCEED() << "spirv-val not installed; skipped";
}

// Regression: a kernel that reads the workgroup size (Workgroup.dimX()) must emit
// VALID Vulkan SPIR-V. The WorkgroupSize BuiltIn must decorate a CONSTANT in a
// Vulkan shader; emitting it as a builtin *variable* (the old spv_workgroup_size
// path) produced SPIR-V that spirv-val rejected ("BuiltIn WorkgroupSize must be a
// constant"), silently breaking any kernel that strides by the block size (e.g.
// the cooperative global→LDS staging copy). workgroupDim now returns the baked
// LocalSize constant.
TEST(XpuVulkanEmitTests, workgroupDimEmitsValidSpirv) {
    auto src =
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.KernelThread;\n"
        "import cajeta.xpu.Workgroup;\n"
        "public class M {\n"
        "    @Kernel\n"
        "    public static void k(KernelBuffer<float32> c) {\n"
        "        uint32 t = KernelThread.x();\n"
        "        uint32 nthr = Workgroup.dimX();\n"
        "        for (uint32 e = t; e < 256; e = e + nthr) { c[e] = (float32) e; }\n"
        "    }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.M");
    auto k = findMethod(module->getStructures()["test.M"], "k");
    ASSERT_NE(k, nullptr);

    auto tm = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext ctx;
    llvm::Module mod("xpu_wgdim_vulkan", ctx);
    configureDeviceModule(mod, *tm);
    lowerKernel(k, mod);
    std::vector<uint8_t> spirv = emitSpirv(mod, *tm);
    ASSERT_FALSE(spirv.empty());
    if (auto valid = validateSpirv(spirv))
        EXPECT_TRUE(*valid) << "Workgroup.dimX() kernel produced invalid SPIR-V";
    else
        GTEST_SUCCEED() << "spirv-val not installed; skipped";
}

// Bindless / multi-buffer descriptor sets (Stage B4): a kernel takes an ARRAY of
// buffers `KernelBuffer<int32>[] bufs` and reads `bufs[b][i]` — the b-th descriptor of
// a runtime descriptor array, then its i-th element. The two-level subscript
// lowers to resource.nonuniformindex(b) (the NonUniformEXT decoration) +
// resource.handlefrombinding (the descriptor array, range = the bindless cap) +
// resource.getpointer (the inner element). Validates as a Vulkan 1.3 module.
TEST(XpuVulkanEmitTests, lowersBindlessBufferArrayToSpirv) {
    auto src =
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.KernelThread;\n"
        "public class M {\n"
        "    @Kernel\n"
        "    public static void gather(KernelBuffer<int32>[] bufs, uint32 count,\n"
        "                              KernelBuffer<int32> out, uint32 n) {\n"
        "        uint32 i = KernelThread.globalIdX();\n"
        "        if (i < n) {\n"
        "            int32 s = 0;\n"
        "            for (uint32 b = 0; b < count; b = b + 1) {\n"
        "                s = s + bufs[b][i];\n"
        "            }\n"
        "            out[i] = s;\n"
        "        }\n"
        "    }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.M");
    auto klass = module->getStructures()["test.M"];
    ASSERT_NE(klass, nullptr);
    auto gather = findMethod(klass, "gather");
    ASSERT_NE(gather, nullptr);

    auto tmIr = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tmIr, nullptr);
    llvm::LLVMContext irCtx;
    llvm::Module irModule("xpu_bindless_ir", irCtx);
    configureDeviceModule(irModule, *tmIr);
    llvm::Function* fn = lowerKernel(gather, irModule);
    ASSERT_NE(fn, nullptr);
    std::string ir = printModule(irModule);
    // The descriptor-array selection: handlefrombinding with the descriptor index
    // (the first subscript) feeding getpointer for the inner element.
    EXPECT_NE(ir.find("llvm.spv.resource.handlefrombinding"), std::string::npos)
        << ir;
    EXPECT_NE(ir.find("llvm.spv.resource.getpointer"), std::string::npos) << ir;

    auto tmText = createSpirvTargetMachine("vulkan1.3");
    llvm::LLVMContext textCtx;
    llvm::Module textModule("xpu_bindless_text", textCtx);
    configureDeviceModule(textModule, *tmText);
    lowerKernel(gather, textModule);
    std::string text = emitSpirvText(textModule, *tmText);
    ASSERT_FALSE(text.empty());
    // A descriptor-array binding (the storage buffers bound as one array).
    EXPECT_NE(text.find("OpEntryPoint GLCompute"), std::string::npos) << text;

    auto tmBin = createSpirvTargetMachine("vulkan1.3");
    llvm::LLVMContext binCtx;
    llvm::Module binModule("xpu_bindless_bin", binCtx);
    configureDeviceModule(binModule, *tmBin);
    lowerKernel(gather, binModule);
    std::vector<uint8_t> spirv = emitSpirv(binModule, *tmBin);
    ASSERT_FALSE(spirv.empty());
    if (auto valid = validateSpirv(spirv))
        EXPECT_TRUE(*valid) << "bindless buffer-array kernel produced invalid SPIR-V";
    else
        GTEST_SUCCEED() << "spirv-val not installed; skipped";
}

// Stage 9: scoped memory fences (Barrier.workgroupMemory / .deviceMemory) lower
// to memory-only OpMemoryBarrier (NOT OpControlBarrier — no thread rendezvous),
// via the dedicated llvm.spv.{group,device}.memory.barrier intrinsics. The proof
// that matters here is spirv-val: OpMemoryBarrier with Vulkan-invalid memory
// semantics (e.g. SequentiallyConsistent) is rejected, so a clean spirv-val
// confirms the semantics-fixup pass covers OpMemoryBarrier too.
TEST(XpuVulkanEmitTests, lowersMemoryFenceToSpirv) {
    auto src =
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.KernelThread;\n"
        "import cajeta.xpu.Barrier;\n"
        "public class MF {\n"
        "    @Kernel\n"
        "    public static void fence(KernelBuffer<int32> data, KernelBuffer<int32> out,\n"
        "                             uint32 n) {\n"
        "        uint32 i = KernelThread.globalIdX();\n"
        "        if (i < n) {\n"
        "            data[i] = (int32)(i * 2);\n"
        "            Barrier.deviceMemory();\n"
        "            Barrier.workgroupMemory();\n"
        "            out[i] = data[i] + 1;\n"
        "        }\n"
        "    }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.MF");
    auto klass = module->getStructures()["test.MF"];
    ASSERT_NE(klass, nullptr);
    auto fence = findMethod(klass, "fence");
    ASSERT_NE(fence, nullptr);

    auto tmIr = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tmIr, nullptr);
    llvm::LLVMContext irCtx;
    llvm::Module irModule("xpu_fence_vulkan_ir", irCtx);
    configureDeviceModule(irModule, *tmIr);
    llvm::Function* fn = lowerKernel(fence, irModule);
    ASSERT_NE(fn, nullptr);
    std::string ir = printModule(irModule);
    EXPECT_NE(ir.find("llvm.spv.device.memory.barrier"), std::string::npos) << ir;
    EXPECT_NE(ir.find("llvm.spv.group.memory.barrier"), std::string::npos) << ir;
    // It is a memory fence, NOT a control barrier — no group-sync intrinsic.
    EXPECT_EQ(ir.find("memory.barrier.with.group.sync"), std::string::npos) << ir;

    auto tmBin = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tmBin, nullptr);
    llvm::LLVMContext binCtx;
    llvm::Module binModule("xpu_fence_vulkan_bin", binCtx);
    configureDeviceModule(binModule, *tmBin);
    lowerKernel(fence, binModule);
    std::string text = emitSpirvText(binModule, *tmBin);
    EXPECT_NE(text.find("OpMemoryBarrier"), std::string::npos) << text;

    auto tmBin2 = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tmBin2, nullptr);
    llvm::LLVMContext bin2Ctx;
    llvm::Module bin2Module("xpu_fence_vulkan_bin2", bin2Ctx);
    configureDeviceModule(bin2Module, *tmBin2);
    lowerKernel(fence, bin2Module);
    std::vector<uint8_t> spirv = emitSpirv(bin2Module, *tmBin2);
    ASSERT_FALSE(spirv.empty());
    if (auto valid = validateSpirv(spirv))
        EXPECT_TRUE(*valid) << "memory-fence kernel produced invalid SPIR-V "
                               "(OpMemoryBarrier semantics not Vulkan-valid?)";
    else
        GTEST_SUCCEED() << "spirv-val not installed; skipped";
}

// Stage 9: the MemoryOrder surface accepts an explicit order on a Vulkan kernel
// atomic and stays spirv-val clean. Vulkan's memory model rejects a bare
// relaxed/seqcst device atomic (it needs storage-class acquire/release
// semantics), so the Vulkan path CLAMPS Relaxed/SeqCst up to AcqRel — the
// kernel here mixes a relaxed int, a default int, and a relaxed float, and all
// three lower to a valid `acq_rel` atomicrmw. (Honouring relaxed natively is a
// CPU/AMD/NVPTX property — see lowersRelaxedAtomicToMonotonicPtx.)
TEST(XpuVulkanEmitTests, lowersAtomicMemoryOrderToSpirv) {
    auto src =
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.KernelThread;\n"
        "import cajeta.xpu.MemoryOrder;\n"
        "public class MO {\n"
        "    @Kernel\n"
        "    public static void bump(KernelBuffer<int32> a, KernelBuffer<int32> b,\n"
        "                            KernelBuffer<float32> c, uint32 n) {\n"
        "        uint32 i = KernelThread.globalIdX();\n"
        "        if (i < n) {\n"
        "            a.atomicAdd(0, 1, MemoryOrder.Relaxed);\n"   // clamped → acq_rel
        "            b.atomicAdd(0, 1);\n"                        // default → acq_rel
        "            c.atomicAdd(0, 1.0f, MemoryOrder.Relaxed);\n"// float, clamped
        "        }\n"
        "    }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.MO");
    auto klass = module->getStructures()["test.MO"];
    ASSERT_NE(klass, nullptr);
    auto bump = findMethod(klass, "bump");
    ASSERT_NE(bump, nullptr);

    auto tmIr = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tmIr, nullptr);
    llvm::LLVMContext irCtx;
    llvm::Module irModule("xpu_memorder_vulkan_ir", irCtx);
    configureDeviceModule(irModule, *tmIr);
    ASSERT_NE(lowerKernel(bump, irModule), nullptr);
    std::string ir = printModule(irModule);
    // All three clamp to acq_rel on Vulkan; none stays monotonic.
    EXPECT_NE(ir.find("acq_rel"), std::string::npos) << ir;
    EXPECT_EQ(ir.find("monotonic"), std::string::npos) << ir;

    auto tmBin = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tmBin, nullptr);
    llvm::LLVMContext binCtx;
    llvm::Module binModule("xpu_memorder_vulkan_bin", binCtx);
    configureDeviceModule(binModule, *tmBin);
    lowerKernel(bump, binModule);
    std::vector<uint8_t> spirv = emitSpirv(binModule, *tmBin);
    ASSERT_FALSE(spirv.empty());
    if (auto valid = validateSpirv(spirv))
        EXPECT_TRUE(*valid) << "memory-order kernel produced invalid SPIR-V "
                               "(Vulkan order clamp not applied?)";
    else
        GTEST_SUCCEED() << "spirv-val not installed; skipped";
}

// Stage 11: `Spec.geti(slot, default)` lowers to a GENUINE OpSpecConstant on
// Vulkan — SpecId kFirstUserSpecId(4)+slot, defaulting to the compile-time
// default — not a baked literal. The witness global is emitted by
// SpirvKernelLowering and rewritten into the spec constant by SpirvBackend's
// post-emit pass (applied in the BINARY path only, so this asserts on the bytes,
// not emitSpirvText). Proven by the SpecId-4 decoration in the binary + spirv-val
// (the Private witness var + spec-constant initializer must validate). The
// runtime leaves SpecId ≥ 4 at its default today (host override = the deferred
// launch contract).
TEST(XpuVulkanEmitTests, lowersSpecConstantToOpSpecConstant) {
    const char* src =
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.KernelThread;\n"
        "public class M {\n"
        "    @Kernel\n"
        "    public static void k(KernelBuffer<int32> out) {\n"
        "        uint32 i = KernelThread.globalIdX();\n"
        "        out[i] = Spec.geti(0, 1234);\n"
        "    }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.M");
    auto klass = module->getStructures()["test.M"];
    ASSERT_NE(klass, nullptr);
    auto k = findMethod(klass, "k");
    ASSERT_NE(k, nullptr);

    auto tmBin = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tmBin, nullptr);
    llvm::LLVMContext binCtx;
    llvm::Module binModule("xpu_spec_vulkan_bin", binCtx);
    configureDeviceModule(binModule, *tmBin);
    ASSERT_NE(lowerKernel(k, binModule), nullptr);
    std::vector<uint8_t> spirv = emitSpirv(binModule, *tmBin);
    ASSERT_FALSE(spirv.empty());

    // A real specialization constant carries a SpecId decoration; SpecId 4 is the
    // first user slot (Spec.geti slot 0). A baked literal would have none.
    EXPECT_TRUE(hasSpecIdDecoration(spirv, /*specId=*/4))
        << "expected an OpSpecConstant with SpecId 4 for Spec.geti(0, …)";

    if (auto valid = validateSpirv(spirv))
        EXPECT_TRUE(*valid) << "spec-constant kernel produced invalid SPIR-V "
                               "(Private witness var / spec-constant init?)";
    else
        GTEST_SUCCEED() << "spirv-val not installed; skipped";
}

// Stage 11 — the f32 companion `Spec.getf(slot, default)` lowers to a genuine
// float OpSpecConstant on Vulkan (SpecId 4 for slot 0), via the same type-
// agnostic witness rewrite as geti — a float witness OpConstant, not a baked
// literal. Proven by the SpecId-4 decoration + spirv-val.
TEST(XpuVulkanEmitTests, lowersFloatSpecConstantToOpSpecConstant) {
    const char* src =
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.KernelThread;\n"
        "public class M {\n"
        "    @Kernel\n"
        "    public static void k(KernelBuffer<float32> out) {\n"
        "        uint32 i = KernelThread.globalIdX();\n"
        "        out[i] = Spec.getf(0, 1.25f);\n"
        "    }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.M");
    auto klass = module->getStructures()["test.M"];
    ASSERT_NE(klass, nullptr);
    auto k = findMethod(klass, "k");
    ASSERT_NE(k, nullptr);

    auto tmBin = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tmBin, nullptr);
    llvm::LLVMContext binCtx;
    llvm::Module binModule("xpu_specf_vulkan_bin", binCtx);
    configureDeviceModule(binModule, *tmBin);
    ASSERT_NE(lowerKernel(k, binModule), nullptr);
    std::vector<uint8_t> spirv = emitSpirv(binModule, *tmBin);
    ASSERT_FALSE(spirv.empty());

    EXPECT_TRUE(hasSpecIdDecoration(spirv, /*specId=*/4))
        << "expected a float OpSpecConstant with SpecId 4 for Spec.getf(0, …)";

    if (auto valid = validateSpirv(spirv))
        EXPECT_TRUE(*valid) << "float spec-constant kernel produced invalid SPIR-V";
    else
        GTEST_SUCCEED() << "spirv-val not installed; skipped";
}
