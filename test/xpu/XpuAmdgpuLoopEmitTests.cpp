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

// A strided-sum loop kernel (identical source to the NVPTX loop test) lowers
// to AMDGPU ISA with a loop backedge and global load/store traffic.

namespace {
// A 2-D texture sampled through a Sampler — the Item 8 Stage C kernel.
const char* kTextureSampleSource =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.gfx.Texture2D;\n"
    "import cajeta.gfx.Sampler;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void sampleTex(Texture2D tex, Sampler s,\n"
    "                                 KernelBuffer<float32> out, uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) { Vector<float32,4> c = tex.sample(s, 0.5, 0.5); out[i] = c.x; }\n"
    "    }\n"
    "}\n";

const char* kTextureFetchSource =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.gfx.Texture2D;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void fetchTex(Texture2D tex,\n"
    "                                KernelBuffer<float32> out, uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) { Vector<float32,4> c = tex.fetch(i, 0); out[i] = c.x; }\n"
    "    }\n"
    "}\n";

// Integer texelFetch (B3 Step 2b): a Texture2D<int32> fetch — the lowerer calls
// __ockl_image_load_2D (v4f32) then bitcasts the raw result to <4 x i32>.
const char* kIntTextureFetchSource =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.gfx.Texture2D;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void fetchTex(Texture2D<int32> tex,\n"
    "                                KernelBuffer<int32> out, uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) { Vector<int32,4> c = tex.fetch(i, 0); out[i] = c.x; }\n"
    "    }\n"
    "}\n";
} // namespace

// Item 8 Stage C: tex.sample lowers to a call to the ROCm device-library
// function __ockl_image_sample_2D, with the texture param typed as a constant
// (addrspace 4) pointer to the HIP texture object. GPU-free: just the IR shape,
// before the device-library link.

// With the ROCm device bitcode present, the device-library link + AMDGPU codegen
// turn that call into a real gfx1151 image_sample instruction (the hardware
// texture path). Gated on a ROCm/HIP install (which is where the device bitcode
// lives); the GPU itself isn't exercised — this is still ISA text.

// texelFetch: tex.fetch lowers to a call to the ROCm device-library function
// __ockl_image_load_2D (the unfiltered twin of __ockl_image_sample_2D), with the
// texture param typed as a constant (addrspace 4) pointer — and NO sampler. IR
// shape only, before the device-library link.

// Integer fetch (B3 Step 2b): a Texture2D<int32> still loads via the only ockl
// 2-D image-load symbol (v4f32-returning), but the lowerer bitcasts the raw
// result to <4 x i32> to recover the integers — the HW image_load is raw on a
// non-normalized integer SRD. GPU-free; just the IR shape.

// B3 texture dims: Texture3D on AMD — fetch lowers to __ockl_image_load_3D and
// sample to __ockl_image_sample_3D (the 3-D ockl twins), each with a 4-component
// coord. GPU-free; just the IR shape, before the device-library link.
TEST(XpuAmdgpuLoopEmitTests, lowers3dTextureToOckl3D) {
    const char* src =
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.gfx.Texture3D;\n"
        "import cajeta.gfx.Sampler;\n"
        "import cajeta.xpu.KernelThread;\n"
        "public class M {\n"
        "    @Kernel\n"
        "    public static void fetchVol(Texture3D vol, KernelBuffer<float32> out, uint32 n) {\n"
        "        uint32 i = KernelThread.globalIdX();\n"
        "        if (i < n) { Vector<float32,4> c = vol.fetch(i, 0, 0); out[i] = c.x; }\n"
        "    }\n"
        "    @Kernel\n"
        "    public static void sampVol(Texture3D vol, Sampler s, KernelBuffer<float32> out, uint32 n) {\n"
        "        uint32 i = KernelThread.globalIdX();\n"
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

// Item 7: a POD struct passed by value as a kernel arg lowers to AMDGPU. The
// kernel takes `Params { int32 mul; int32 add; }` by value (an aggregate kernarg)
// and reads its fields to compute out[i] = i*mul + add.
