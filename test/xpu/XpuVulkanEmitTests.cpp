//
// CajetaXPU Vulkan bring-up — SPIR-V emission + kernel-body lowering through
// the SHARED AST walk with the SPIR-V (Vulkan) LoweringTarget.
//
// This is the third backend's seam payoff — and the measured proof that Vulkan
// forks MORE than AMD did (cajeta-xpu-matrix.md): the SAME shared AST walk
// lowers the kernel body, but the SPIR-V LoweringTarget forks the kernel
// SIGNATURE (a `void main()` GLCompute entry, no params) and BUFFER ACCESS
// (descriptor-set storage buffers via resource.handlefrombinding/getpointer,
// since LLVM 22's SPIR-V backend has no raw-pointer kernel ABI and no BDA).
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

#include "llvm/IR/Function.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

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
        "import cajeta.xpu.core.Buffer;\n"
        "import cajeta.xpu.core.Thread;\n"
        "public class M {\n"
        "    @Kernel\n"
        "    public static void saxpy(Buffer<float32> y, Buffer<float32> x,\n"
        "                              float32 a, uint32 n) {\n"
        "        uint32 i = Thread.globalIdX();\n"
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
        "import cajeta.xpu.core.Buffer;\n"
        "import cajeta.xpu.core.Thread;\n"
        "public class Params {\n"
        "    int32 mul;\n"
        "    int32 add;\n"
        "    public Params(int32 mul, int32 add)"
        " { this.mul = mul; this.add = add; }\n"
        "}\n"
        "public class M {\n"
        "    @Kernel\n"
        "    public static void k(Buffer<int32> out, Params p) {\n"
        "        uint32 i = Thread.globalIdX();\n"
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
        "import cajeta.xpu.core.Buffer;\n"
        "import cajeta.xpu.core.Thread;\n"
        "public class M {\n"
        "    @Kernel\n"
        "    public static void strideSum(Buffer<int32> out, Buffer<int32> in,\n"
        "                                  uint32 n, uint32 stride) {\n"
        "        uint32 i = Thread.globalIdX();\n"
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
        "import cajeta.xpu.core.Buffer;\n"
        "import cajeta.xpu.core.Thread;\n"
        "import cajeta.xpu.core.Texture2D;\n"
        "import cajeta.xpu.core.Sampler;\n"
        "public class M {\n"
        "    @Kernel\n"
        "    public static void sampleTex(Texture2D tex, Sampler s,\n"
        "                                 Buffer<float32> out, uint32 n) {\n"
        "        uint32 i = Thread.globalIdX();\n"
        "        if (i < n) {\n"
        "            out[i] = tex.sample(s, 0.5, 0.5);\n"
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

// A workgroup-shared kernel with a barrier emits a SPIR-V module that is
// strictly Vulkan-VALID — proving the barrier post-pass (SpirvBackend's
// fixupControlBarriers) rewrites LLVM 22's forbidden SequentiallyConsistent
// OpControlBarrier semantics to WorkgroupMemory|AcquireRelease. Without the
// fixup this module fails `spirv-val` (VUID-StandaloneSpirv-MemorySemantics).
TEST(XpuVulkanEmitTests, workgroupBarrierIsSpecValid) {
    auto src =
        "package test;\n"
        "import cajeta.xpu.core.Buffer;\n"
        "import cajeta.xpu.core.Thread;\n"
        "import cajeta.xpu.core.Barrier;\n"
        "import cajeta.xpu.core.Shared;\n"
        "public class M {\n"
        "    @Kernel\n"
        "    public static void reduce(Buffer<int32> out, Buffer<int32> in) {\n"
        "        Shared<int32> tile = shared int32[64];\n"
        "        uint32 t = Thread.x();\n"
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

    auto valid = validateSpirv(spirv);
    if (!valid) {
        GTEST_SUCCEED() << "spirv-val not installed; barrier validity unchecked";
        return;
    }
    EXPECT_TRUE(*valid) << "barrier kernel failed spirv-val — the OpControlBarrier "
                           "semantics fixup did not take effect";
}
