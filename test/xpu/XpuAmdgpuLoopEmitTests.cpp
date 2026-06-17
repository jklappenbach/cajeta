//
// CajetaXPU AMD bring-up (cajeta-amd.md Increment 2) — kernel-body lowering
// through the SHARED AST walk with the AMDGPU LoweringTarget.
//
// This is the seam payoff: the SAME ~885-line AST walk that lowers NVPTX
// kernels lowers AMDGPU ones, with only LoweringTarget forking. The SAXPY +
// strided-loop sources here are byte-identical to the NVPTX emit tests; only
// the backend differs. Two assertion layers:
//   - IR structure (deterministic): the measured seam decisions — alloca in
//     addrspace(5), buffers in addrspace(1), amdgcn coordinate intrinsics,
//     the dispatch-packet block-dim read, amdgpu_kernel calling convention.
//   - ISA smoke: end-to-end AMDGPU codegen produces real gfx1151 ISA.
//
// GPU-free (ISA is text). On-device verification is XpuSaxpyAmdDeviceTests.
//

#include "gtest/gtest.h"

#include "cajeta/xpu/amd/AmdgpuBackend.h"
#include "cajeta/xpu/amd/AmdgpuKernelLowering.h"
#include "XpuDeviceTestUtil.h"

#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/method/Method.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

using namespace cajeta::xpu::amd;
using cajeta::Compiler;
using cajeta::CajetaModulePtr;

namespace {

CajetaModulePtr compileForInspection(Compiler& compiler,
                                     const std::string& source,
                                     const std::string& fqClassName) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_xpu_amdgpu_" + std::to_string(rng()));
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
                 / ("cajeta_xpu_amdgpu_arch_" + std::to_string(rng()));
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

} // namespace

// SAXPY lowers through the shared walk + AMDGPU target: the IR carries every
// measured seam decision, and the AMDGPU backend emits real gfx1151 ISA.
TEST(XpuAmdgpuLoopEmitTests, lowersSaxpyToIsaAndIr) {
    auto src =
        "package test;\n"
        "import cajeta.gpu.GpuBuffer;\n"
        "import cajeta.gpu.GpuThread;\n"
        "public class M {\n"
        "    @Kernel\n"
        "    public static void saxpy(GpuBuffer<float32> y, GpuBuffer<float32> x,\n"
        "                              float32 a, uint32 n) {\n"
        "        uint32 i = GpuThread.globalIdX();\n"
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

    auto tm = createAmdgpuTargetMachine("gfx1151");
    ASSERT_NE(tm, nullptr);

    llvm::LLVMContext deviceCtx;
    llvm::Module deviceModule("xpu_saxpy_amdgpu", deviceCtx);
    configureDeviceModule(deviceModule, *tm);

    llvm::Function* fn = lowerKernel(saxpy, deviceModule);
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn->arg_size(), 4u);
    // Seam: AMDGPU kernels are marked by calling convention, no metadata.
    EXPECT_EQ(fn->getCallingConv(), llvm::CallingConv::AMDGPU_KERNEL);

    std::string ir = printModule(deviceModule);
    // Seam: mutable scalar slots allocate in the private address space (5).
    EXPECT_NE(ir.find("addrspace(5)"), std::string::npos) << ir;
    // GpuBuffer params are device global memory (addrspace(1)).
    EXPECT_NE(ir.find("ptr addrspace(1)"), std::string::npos) << ir;
    // Coordinate reads use the amdgcn intrinsics; global id needs workitem +
    // workgroup id AND the dispatch-packet block-dim read.
    EXPECT_NE(ir.find("llvm.amdgcn.workitem.id.x"), std::string::npos) << ir;
    EXPECT_NE(ir.find("llvm.amdgcn.workgroup.id.x"), std::string::npos) << ir;
    EXPECT_NE(ir.find("llvm.amdgcn.dispatch.ptr"), std::string::npos) << ir;

    std::string isa = emitIsa(deviceModule, *tm);
    ASSERT_FALSE(isa.empty());
    EXPECT_NE(isa.find(".amdhsa_kernel saxpy"), std::string::npos) << isa;
    EXPECT_NE(isa.find("global_load"), std::string::npos) << isa;
    EXPECT_NE(isa.find("global_store"), std::string::npos) << isa;
    EXPECT_NE(isa.find("s_endpgm"), std::string::npos) << isa;
    EXPECT_NE(isa.find("gfx1151"), std::string::npos) << isa;
}

// A strided-sum loop kernel (identical source to the NVPTX loop test) lowers
// to AMDGPU ISA with a loop backedge and global load/store traffic.
TEST(XpuAmdgpuLoopEmitTests, lowersStridedSumLoop) {
    auto src =
        "package test;\n"
        "import cajeta.gpu.GpuBuffer;\n"
        "import cajeta.gpu.GpuThread;\n"
        "public class M {\n"
        "    @Kernel\n"
        "    public static void strideSum(GpuBuffer<int32> out, GpuBuffer<int32> in,\n"
        "                                  uint32 n, uint32 stride) {\n"
        "        uint32 i = GpuThread.globalIdX();\n"
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

    auto tm = createAmdgpuTargetMachine("gfx1151");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext deviceCtx;
    llvm::Module deviceModule("xpu_stridesum_amdgpu", deviceCtx);
    configureDeviceModule(deviceModule, *tm);
    llvm::Function* fn = lowerKernel(strideSum, deviceModule);
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn->getCallingConv(), llvm::CallingConv::AMDGPU_KERNEL);

    std::string isa = emitIsa(deviceModule, *tm);
    ASSERT_FALSE(isa.empty());
    EXPECT_NE(isa.find(".amdhsa_kernel strideSum"), std::string::npos) << isa;
    // A loop: at least one conditional/uniform branch (the backedge / guard).
    EXPECT_TRUE(isa.find("s_cbranch") != std::string::npos ||
                isa.find("s_branch") != std::string::npos) << isa;
    EXPECT_NE(isa.find("global_load"), std::string::npos) << isa;
    EXPECT_NE(isa.find("global_store"), std::string::npos) << isa;
    EXPECT_NE(isa.find("s_endpgm"), std::string::npos) << isa;
}

namespace {
// A 2-D texture sampled through a Sampler — the Item 8 Stage C kernel.
const char* kTextureSampleSource =
    "package test;\n"
    "import cajeta.gpu.GpuBuffer;\n"
    "import cajeta.gpu.Texture2D;\n"
    "import cajeta.gpu.Sampler;\n"
    "import cajeta.gpu.GpuThread;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void sampleTex(Texture2D tex, Sampler s,\n"
    "                                 GpuBuffer<float32> out, uint32 n) {\n"
    "        uint32 i = GpuThread.globalIdX();\n"
    "        if (i < n) { Vector<float32,4> c = tex.sample(s, 0.5, 0.5); out[i] = c.x; }\n"
    "    }\n"
    "}\n";

const char* kTextureFetchSource =
    "package test;\n"
    "import cajeta.gpu.GpuBuffer;\n"
    "import cajeta.gpu.Texture2D;\n"
    "import cajeta.gpu.GpuThread;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void fetchTex(Texture2D tex,\n"
    "                                GpuBuffer<float32> out, uint32 n) {\n"
    "        uint32 i = GpuThread.globalIdX();\n"
    "        if (i < n) { Vector<float32,4> c = tex.fetch(i, 0); out[i] = c.x; }\n"
    "    }\n"
    "}\n";

// Integer texelFetch (B3 Step 2b): a Texture2D<int32> fetch — the lowerer calls
// __ockl_image_load_2D (v4f32) then bitcasts the raw result to <4 x i32>.
const char* kIntTextureFetchSource =
    "package test;\n"
    "import cajeta.gpu.GpuBuffer;\n"
    "import cajeta.gpu.Texture2D;\n"
    "import cajeta.gpu.GpuThread;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void fetchTex(Texture2D<int32> tex,\n"
    "                                GpuBuffer<int32> out, uint32 n) {\n"
    "        uint32 i = GpuThread.globalIdX();\n"
    "        if (i < n) { Vector<int32,4> c = tex.fetch(i, 0); out[i] = c.x; }\n"
    "    }\n"
    "}\n";
} // namespace

// Item 8 Stage C: tex.sample lowers to a call to the ROCm device-library
// function __ockl_image_sample_2D, with the texture param typed as a constant
// (addrspace 4) pointer to the HIP texture object. GPU-free: just the IR shape,
// before the device-library link.
TEST(XpuAmdgpuLoopEmitTests, lowersTextureSampleToOcklCall) {
    Compiler compiler;
    auto module = compileForInspection(compiler, kTextureSampleSource, "test.M");
    auto k = findMethod(module->getStructures()["test.M"], "sampleTex");
    ASSERT_NE(k, nullptr);

    auto tm = createAmdgpuTargetMachine("gfx1151");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext deviceCtx;
    llvm::Module deviceModule("xpu_texsample_amdgpu_ir", deviceCtx);
    configureDeviceModule(deviceModule, *tm);
    llvm::Function* fn = lowerKernel(k, deviceModule);
    ASSERT_NE(fn, nullptr);

    std::string ir = printModule(deviceModule);
    // The texture param is a constant-AS pointer (the HIP texture object).
    EXPECT_NE(ir.find("ptr addrspace(4)"), std::string::npos) << ir;
    // The sample call targets the ROCm device-library explicit-LOD image-sample
    // function (the seam always uses the _lod variant; plain sample passes lod 0).
    EXPECT_NE(ir.find("__ockl_image_sample_lod_2D"), std::string::npos) << ir;
}

// With the ROCm device bitcode present, the device-library link + AMDGPU codegen
// turn that call into a real gfx1151 image_sample instruction (the hardware
// texture path). Gated on a ROCm/HIP install (which is where the device bitcode
// lives); the GPU itself isn't exercised — this is still ISA text.
TEST(XpuAmdgpuLoopEmitTests, textureSampleEmitsImageSampleIsa) {
    CAJETA_SKIP_IF_NO_HIP();
    Compiler compiler;
    auto module = compileForInspection(compiler, kTextureSampleSource, "test.M");
    auto k = findMethod(module->getStructures()["test.M"], "sampleTex");
    ASSERT_NE(k, nullptr);

    auto tm = createAmdgpuTargetMachine("gfx1151");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext deviceCtx;
    llvm::Module deviceModule("xpu_texsample_amdgpu_isa", deviceCtx);
    configureDeviceModule(deviceModule, *tm);
    ASSERT_NE(lowerKernel(k, deviceModule), nullptr);

    std::string isa = emitIsa(deviceModule, *tm);   // links ockl.bc internally
    ASSERT_FALSE(isa.empty());
    EXPECT_NE(isa.find(".amdhsa_kernel sampleTex"), std::string::npos) << isa;
    // The gfx11 hardware image-sample (dim:SQ_RSRC_IMG_2D) from the linked
    // __ockl_image_sample_2D — proof the device-library path reached real ISA.
    EXPECT_NE(isa.find("image_sample"), std::string::npos) << isa;
}

// texelFetch: tex.fetch lowers to a call to the ROCm device-library function
// __ockl_image_load_2D (the unfiltered twin of __ockl_image_sample_2D), with the
// texture param typed as a constant (addrspace 4) pointer — and NO sampler. IR
// shape only, before the device-library link.
TEST(XpuAmdgpuLoopEmitTests, lowersTextureFetchToOcklLoad) {
    Compiler compiler;
    auto module = compileForInspection(compiler, kTextureFetchSource, "test.M");
    auto k = findMethod(module->getStructures()["test.M"], "fetchTex");
    ASSERT_NE(k, nullptr);

    auto tm = createAmdgpuTargetMachine("gfx1151");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext deviceCtx;
    llvm::Module deviceModule("xpu_texfetch_amdgpu_ir", deviceCtx);
    configureDeviceModule(deviceModule, *tm);
    llvm::Function* fn = lowerKernel(k, deviceModule);
    ASSERT_NE(fn, nullptr);

    std::string ir = printModule(deviceModule);
    EXPECT_NE(ir.find("ptr addrspace(4)"), std::string::npos) << ir;
    EXPECT_NE(ir.find("__ockl_image_load_lod_2D"), std::string::npos) << ir;
    EXPECT_EQ(ir.find("__ockl_image_sample_lod_2D"), std::string::npos) << ir;
}

// Integer fetch (B3 Step 2b): a Texture2D<int32> still loads via the only ockl
// 2-D image-load symbol (v4f32-returning), but the lowerer bitcasts the raw
// result to <4 x i32> to recover the integers — the HW image_load is raw on a
// non-normalized integer SRD. GPU-free; just the IR shape.
TEST(XpuAmdgpuLoopEmitTests, intTextureFetchBitcastsToI32) {
    Compiler compiler;
    auto module = compileForInspection(compiler, kIntTextureFetchSource, "test.M");
    auto k = findMethod(module->getStructures()["test.M"], "fetchTex");
    ASSERT_NE(k, nullptr);

    auto tm = createAmdgpuTargetMachine("gfx1151");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext deviceCtx;
    llvm::Module deviceModule("xpu_texfetch_int_amdgpu_ir", deviceCtx);
    configureDeviceModule(deviceModule, *tm);
    llvm::Function* fn = lowerKernel(k, deviceModule);
    ASSERT_NE(fn, nullptr);

    std::string ir = printModule(deviceModule);
    EXPECT_NE(ir.find("__ockl_image_load_lod_2D"), std::string::npos) << ir;
    // The raw v4f32 image-load result reinterpreted as integers.
    EXPECT_NE(ir.find("bitcast <4 x float>"), std::string::npos) << ir;
    EXPECT_NE(ir.find("to <4 x i32>"), std::string::npos) << ir;
}

// B3 texture dims: Texture3D on AMD — fetch lowers to __ockl_image_load_3D and
// sample to __ockl_image_sample_3D (the 3-D ockl twins), each with a 4-component
// coord. GPU-free; just the IR shape, before the device-library link.
TEST(XpuAmdgpuLoopEmitTests, lowers3dTextureToOckl3D) {
    const char* src =
        "package test;\n"
        "import cajeta.gpu.GpuBuffer;\n"
        "import cajeta.gpu.Texture3D;\n"
        "import cajeta.gpu.Sampler;\n"
        "import cajeta.gpu.GpuThread;\n"
        "public class M {\n"
        "    @Kernel\n"
        "    public static void fetchVol(Texture3D vol, GpuBuffer<float32> out, uint32 n) {\n"
        "        uint32 i = GpuThread.globalIdX();\n"
        "        if (i < n) { Vector<float32,4> c = vol.fetch(i, 0, 0); out[i] = c.x; }\n"
        "    }\n"
        "    @Kernel\n"
        "    public static void sampVol(Texture3D vol, Sampler s, GpuBuffer<float32> out, uint32 n) {\n"
        "        uint32 i = GpuThread.globalIdX();\n"
        "        if (i < n) { Vector<float32,4> c = vol.sample(s, 0.5f, 0.5f, 0.5f); out[i] = c.x; }\n"
        "    }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.M");
    auto tm = createAmdgpuTargetMachine("gfx1151");
    ASSERT_NE(tm, nullptr);

    {
        auto k = findMethod(module->getStructures()["test.M"], "fetchVol");
        ASSERT_NE(k, nullptr);
        llvm::LLVMContext ctx;
        llvm::Module dm("xpu_tex3d_fetch_amdgpu", ctx);
        configureDeviceModule(dm, *tm);
        ASSERT_NE(lowerKernel(k, dm), nullptr);
        std::string ir = printModule(dm);
        EXPECT_NE(ir.find("__ockl_image_load_3D"), std::string::npos) << ir;
    }
    {
        auto k = findMethod(module->getStructures()["test.M"], "sampVol");
        ASSERT_NE(k, nullptr);
        llvm::LLVMContext ctx;
        llvm::Module dm("xpu_tex3d_sample_amdgpu", ctx);
        configureDeviceModule(dm, *tm);
        ASSERT_NE(lowerKernel(k, dm), nullptr);
        std::string ir = printModule(dm);
        EXPECT_NE(ir.find("__ockl_image_sample_3D"), std::string::npos) << ir;
    }
}

// With the ROCm device bitcode present, the link + AMDGPU codegen turn the fetch
// call into a real gfx1151 image_load instruction (the hardware texel-fetch
// path). Gated on a ROCm/HIP install; the GPU itself isn't exercised — ISA text.
TEST(XpuAmdgpuLoopEmitTests, textureFetchEmitsImageLoadIsa) {
    CAJETA_SKIP_IF_NO_HIP();
    Compiler compiler;
    auto module = compileForInspection(compiler, kTextureFetchSource, "test.M");
    auto k = findMethod(module->getStructures()["test.M"], "fetchTex");
    ASSERT_NE(k, nullptr);

    auto tm = createAmdgpuTargetMachine("gfx1151");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext deviceCtx;
    llvm::Module deviceModule("xpu_texfetch_amdgpu_isa", deviceCtx);
    configureDeviceModule(deviceModule, *tm);
    ASSERT_NE(lowerKernel(k, deviceModule), nullptr);

    std::string isa = emitIsa(deviceModule, *tm);   // links ockl.bc internally
    ASSERT_FALSE(isa.empty());
    EXPECT_NE(isa.find(".amdhsa_kernel fetchTex"), std::string::npos) << isa;
    // The gfx11 hardware image-load from the linked __ockl_image_load_2D.
    EXPECT_NE(isa.find("image_load"), std::string::npos) << isa;
}

// Item 7: a POD struct passed by value as a kernel arg lowers to AMDGPU. The
// kernel takes `Params { int32 mul; int32 add; }` by value (an aggregate kernarg)
// and reads its fields to compute out[i] = i*mul + add.
TEST(XpuAmdgpuLoopEmitTests, lowersPodStructArg) {
    auto src =
        "package test;\n"
        "import cajeta.gpu.GpuBuffer;\n"
        "import cajeta.gpu.GpuThread;\n"
        "public class Params {\n"
        "    int32 mul;\n"
        "    int32 add;\n"
        "    public Params(int32 mul, int32 add)"
        " { this.mul = mul; this.add = add; }\n"
        "}\n"
        "public class M {\n"
        "    @Kernel\n"
        "    public static void k(GpuBuffer<int32> out, Params p) {\n"
        "        uint32 i = GpuThread.globalIdX();\n"
        "        out[i] = (int32)i * p.mul + p.add;\n"
        "    }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.M");
    auto k = findMethod(module->getStructures()["test.M"], "k");
    ASSERT_NE(k, nullptr);

    auto tm = createAmdgpuTargetMachine("gfx1151");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext deviceCtx;
    llvm::Module deviceModule("xpu_podstruct_amdgpu", deviceCtx);
    configureDeviceModule(deviceModule, *tm);
    llvm::Function* fn = lowerKernel(k, deviceModule);
    ASSERT_NE(fn, nullptr);
    // Two kernel params: the out buffer + the struct by value.
    EXPECT_EQ(fn->arg_size(), 2u);
    EXPECT_EQ(fn->getCallingConv(), llvm::CallingConv::AMDGPU_KERNEL);

    std::string isa = emitIsa(deviceModule, *tm);
    ASSERT_FALSE(isa.empty());
    EXPECT_NE(isa.find(".amdhsa_kernel k"), std::string::npos) << isa;
    EXPECT_NE(isa.find("global_store"), std::string::npos) << isa;
    EXPECT_NE(isa.find("s_endpgm"), std::string::npos) << isa;
    EXPECT_NE(isa.find("gfx1151"), std::string::npos) << isa;
}
