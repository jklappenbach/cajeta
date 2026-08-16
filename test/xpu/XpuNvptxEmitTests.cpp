//
// CajetaXPU step 8 (increment C) — NVPTX PTX emission.
//
// First proof point: LLVM 23's in-tree NVPTX backend can lower a device
// llvm::Module to PTX text in THIS build (monolithic libLLVM, MinGW).
// This isolates the toolchain risk (does NVPTX emit at all?) from the
// later kernel-body-lowering risk — so it hand-builds a minimal kernel
// module rather than going through Cajeta AST lowering.
//
// No GPU required: PTX is text. ptxas assembly (cubin) is a later test.
//

#include "gtest/gtest.h"

#include "cajeta/xpu/nvidia/NvptxBackend.h"
#include "cajeta/xpu/nvidia/NvptxKernelLowering.h"

#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/method/Method.h"

#include "../jit/JitTestHelper.h"     // CajetaJit — the hardware-gated device test
#include "XpuDeviceTestUtil.h"        // CAJETA_SKIP_IF_NO_CUDA()

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/IntrinsicsNVPTX.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Target/TargetMachine.h"

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

using namespace cajeta::xpu::nvidia;
using cajeta::Compiler;
using cajeta::CajetaModulePtr;

namespace {

// Parse-only compile (no codegen) — gives us a resolved @Kernel method
// to lower. Same pattern as the other XPU inspection tests.
CajetaModulePtr compileForInspection(Compiler& compiler,
                                     const std::string& source,
                                     const std::string& fqClassName) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_xpu_nvptx_" + std::to_string(rng()));
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
                 / ("cajeta_xpu_nvptx_arch_" + std::to_string(rng()));
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

// Hand-build a minimal NVPTX kernel:
//   ptx_kernel void probe(ptr addrspace(1) out) {
//     out[tid.x] = 1.0f;
//   }
// plus the nvvm.annotations "kernel" marker. Returns the function name.
std::string buildProbeKernel(llvm::Module& m, llvm::LLVMContext& ctx) {
    auto* f32 = llvm::Type::getFloatTy(ctx);
    auto* i64 = llvm::Type::getInt64Ty(ctx);
    auto* voidTy = llvm::Type::getVoidTy(ctx);
    auto* globalPtr = llvm::PointerType::get(ctx, /*addrspace=*/1);

    auto* fnTy = llvm::FunctionType::get(voidTy, {globalPtr}, /*vararg=*/false);
    auto* fn = llvm::Function::Create(
        fnTy, llvm::Function::ExternalLinkage, "probe", &m);
    fn->setCallingConv(llvm::CallingConv::PTX_Kernel);

    llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx, "entry", fn));
    llvm::Function* tidX = llvm::Intrinsic::getOrInsertDeclaration(
        &m, llvm::Intrinsic::nvvm_read_ptx_sreg_tid_x);
    llvm::Value* tid = b.CreateCall(tidX, {}, "tid");
    llvm::Value* idx = b.CreateZExt(tid, i64);
    llvm::Value* gep = b.CreateGEP(f32, fn->getArg(0), {idx}, "p");
    b.CreateStore(llvm::ConstantFP::get(f32, 1.0), gep);
    b.CreateRetVoid();

    // nvvm.annotations: { ptr @probe, "kernel", i32 1 }
    auto* i32 = llvm::Type::getInt32Ty(ctx);
    llvm::Metadata* ops[] = {
        llvm::ValueAsMetadata::get(fn),
        llvm::MDString::get(ctx, "kernel"),
        llvm::ValueAsMetadata::get(llvm::ConstantInt::get(i32, 1)),
    };
    m.getOrInsertNamedMetadata("nvvm.annotations")
        ->addOperand(llvm::MDNode::get(ctx, ops));

    return "probe";
}

} // namespace

// The nvptx64 target is registered in this LLVM build and yields a
// usable TargetMachine.
TEST(XpuNvptxEmitTests, nvptxTargetMachineAvailable) {
    auto tm = createNvptxTargetMachine("sm_89");
    ASSERT_NE(tm, nullptr) << "nvptx64 target not built into this LLVM";
}

// A hand-built device kernel lowers to PTX containing an entry point,
// the kernel name, and a thread-id special-register read.
TEST(XpuNvptxEmitTests, emitsPtxForHandBuiltKernel) {
    auto tm = createNvptxTargetMachine("sm_89");
    ASSERT_NE(tm, nullptr);

    llvm::LLVMContext ctx;
    llvm::Module m("xpu_nvptx_probe", ctx);
    configureDeviceModule(m, *tm);
    std::string name = buildProbeKernel(m, ctx);

    std::string ptx = emitPtx(m, *tm);
    ASSERT_FALSE(ptx.empty()) << "NVPTX emitted no PTX";

    // A PTX module header, a visible entry for our kernel, and a tid read.
    EXPECT_NE(ptx.find(".visible .entry"), std::string::npos) << ptx;
    EXPECT_NE(ptx.find(name), std::string::npos) << ptx;
    EXPECT_NE(ptx.find("%tid.x"), std::string::npos) << ptx;
    // Sanity: the PTX target arch matches what we asked for.
    EXPECT_NE(ptx.find(".target sm_89"), std::string::npos) << ptx;
}

// The real SAXPY @Kernel source lowers (signature + body) to a device
// function and emits PTX with the expected structure: a kernel entry,
// the global-thread-index reads, global loads/stores, and FP math.
TEST(XpuNvptxEmitTests, lowersSaxpyKernelToPtx) {
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

    auto tm = createNvptxTargetMachine("sm_89");
    ASSERT_NE(tm, nullptr);

    // Device module gets its OWN LLVMContext — the CajetaType llvm::Type
    // cache is bound to the host context, so the lowerer must build device
    // types fresh (it does, from CajetaType flags). This test proves that
    // cross-context lowering produces valid IR.
    llvm::LLVMContext deviceCtx;
    llvm::Module deviceModule("xpu_saxpy_device", deviceCtx);
    configureDeviceModule(deviceModule, *tm);

    llvm::Function* fn = lowerKernel(saxpy, deviceModule);
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn->arg_size(), 4u);

    std::string ptx = emitPtx(deviceModule, *tm);
    ASSERT_FALSE(ptx.empty());

    EXPECT_NE(ptx.find(".visible .entry saxpy"), std::string::npos) << ptx;
    // Global thread index = ctaid.x * ntid.x + tid.x.
    EXPECT_NE(ptx.find("%tid.x"), std::string::npos) << ptx;
    EXPECT_NE(ptx.find("%ntid.x"), std::string::npos) << ptx;
    EXPECT_NE(ptx.find("%ctaid.x"), std::string::npos) << ptx;
    // `if (i < n)` lowers to an UNSIGNED compare + guarded branch (uint32).
    EXPECT_NE(ptx.find("setp.ge.u32"), std::string::npos) << ptx;
    EXPECT_NE(ptx.find("bra"), std::string::npos) << ptx;
    // KernelBuffer params land in global memory (opaque 32-bit loads emit as .b32).
    EXPECT_NE(ptx.find("ld.global"), std::string::npos) << ptx;
    EXPECT_NE(ptx.find("st.global"), std::string::npos) << ptx;
    // The a*x+y float math: separate multiply + add (no fast-math
    // contraction, so it stays IEEE-correct rather than fusing to FMA).
    EXPECT_NE(ptx.find("mul.rn.f32"), std::string::npos) << ptx;
    EXPECT_NE(ptx.find("add.rn.f32"), std::string::npos) << ptx;
}

// Item 8 Stage D (NVIDIA, emit-only): tex.sample lowers to the NVPTX unified
// texture fetch — the i64 cudaTextureObject_t is sampled via
// llvm.nvvm.tex.unified.2d.v4f32.f32, which emits the PTX `tex.2d` instruction.
// No NVIDIA hardware here, so this is PTX text only (the on-device dispatch is
// GTEST_SKIPped like the other NVPTX device tests).
TEST(XpuNvptxEmitTests, lowersTextureSampleToPtxTex) {
    auto src =
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
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.M");
    auto k = findMethod(module->getStructures()["test.M"], "sampleTex");
    ASSERT_NE(k, nullptr);

    auto tm = createNvptxTargetMachine("sm_89");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext deviceCtx;
    llvm::Module deviceModule("xpu_texsample_nvptx", deviceCtx);
    configureDeviceModule(deviceModule, *tm);
    llvm::Function* fn = lowerKernel(k, deviceModule);
    ASSERT_NE(fn, nullptr);

    // The IR carries the unified texture-fetch intrinsic; the texture param is
    // the i64 cudaTextureObject_t handle.
    std::string ir;
    { llvm::raw_string_ostream os(ir); deviceModule.print(os, nullptr); }
    EXPECT_NE(ir.find("llvm.nvvm.tex.unified.2d.v4f32.f32"),
              std::string::npos) << ir;

    std::string ptx = emitPtx(deviceModule, *tm);
    ASSERT_FALSE(ptx.empty());
    EXPECT_NE(ptx.find(".visible .entry sampleTex"), std::string::npos) << ptx;
    // The hardware texture fetch: PTX `tex.2d.v4.f32.f32`.
    EXPECT_NE(ptx.find("tex.2d"), std::string::npos) << ptx;
}

// Image2D storage images on NVIDIA (emit-only core, the writable twin of the
// texture path): img.store/img.load lower to the NVPTX surface store/load —
// llvm.nvvm.sust.b.2d.i32.trap / llvm.nvvm.suld.2d.i32.trap, which emit the PTX
// `sust.b.2d` / `suld.b.2d` instructions. The i64 cudaSurfaceObject_t rides the
// kernarg; x is byte-scaled (×4 for the R32 texel) and the f32 texel is carried
// as raw i32 bits. No NVIDIA hardware here — this is the PTX-text proof; the
// device run + CUDA surface runtime land with B5.
TEST(XpuNvptxEmitTests, lowersImageStoreLoadToPtxSurf) {
    auto src =
        "package test;\n"
        "import cajeta.xpu.Image2D;\n"
        "import cajeta.xpu.KernelThread;\n"
        "public class M {\n"
        "    @Kernel\n"
        "    public static void rmw(Image2D img, uint32 n) {\n"
        "        uint32 i = KernelThread.globalIdX();\n"
        "        if (i < n) {\n"
        "            float32 v = img.load(i, 0);\n"
        "            img.store(i, 0, 2.0f * v + 1.0f);\n"
        "        }\n"
        "    }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.M");
    auto k = findMethod(module->getStructures()["test.M"], "rmw");
    ASSERT_NE(k, nullptr);

    auto tm = createNvptxTargetMachine("sm_89");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext deviceCtx;
    llvm::Module deviceModule("xpu_imgrmw_nvptx", deviceCtx);
    configureDeviceModule(deviceModule, *tm);
    llvm::Function* fn = lowerKernel(k, deviceModule);
    ASSERT_NE(fn, nullptr);

    // The IR carries both surface intrinsics; the image param is the i64 surface
    // handle (the byte-offset x*4 shows as a `shl ... 2`).
    std::string ir;
    { llvm::raw_string_ostream os(ir); deviceModule.print(os, nullptr); }
    EXPECT_NE(ir.find("llvm.nvvm.sust.b.2d.i32"), std::string::npos) << ir;
    EXPECT_NE(ir.find("llvm.nvvm.suld.2d.i32"), std::string::npos) << ir;

    std::string ptx = emitPtx(deviceModule, *tm);
    ASSERT_FALSE(ptx.empty());
    EXPECT_NE(ptx.find(".visible .entry rmw"), std::string::npos) << ptx;
    // The hardware surface store + load.
    EXPECT_NE(ptx.find("sust.b.2d"), std::string::npos) << ptx;
    EXPECT_NE(ptx.find("suld.b.2d"), std::string::npos) << ptx;
}

// Hardware-modulated companion: the full fill→RMW→download (2i+1) on a real CUDA
// device, gated by CAJETA_SKIP_IF_NO_CUDA(). SKIPs here (no CUDA). It is safe
// before the B5 CUDA surface runtime exists: __cajeta_xpu_image_alloc returns 0
// for CUDA, the launch guard refuses to dispatch an unbacked surface, the image
// stays at the -1.0 sentinel, and run() returns 555 → SKIP (never a false fail
// or a fault). It turns green at B5 when the surface runtime is wired.
TEST(XpuNvptxEmitTests, imageLoadStoreRmwOnNvptx) {
    CAJETA_SKIP_IF_NO_CUDA();
    const char* src =
        "package test;\n"
        "import cajeta.xpu.Image2D;\n"
        "import cajeta.xpu.KernelStream;\n"
        "import cajeta.xpu.KernelThread;\n"
        "public class ImgRmwNv {\n"
        "    @Kernel\n"
        "    public static void fill(Image2D img, uint32 w, uint32 h) {\n"
        "        uint32 i = KernelThread.globalIdX();\n"
        "        if (i < w * h) { img.store(i % w, i / w, (float32)(i / w * w + i % w)); }\n"
        "    }\n"
        "    @Kernel\n"
        "    public static void rmw(Image2D img, uint32 w, uint32 h) {\n"
        "        uint32 i = KernelThread.globalIdX();\n"
        "        if (i < w * h) {\n"
        "            uint32 x = i % w;\n"
        "            uint32 y = i / w;\n"
        "            float32 v = img.load(x, y);\n"
        "            img.store(x, y, 2.0f * v + 1.0f);\n"
        "        }\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        uint32 w = 4;\n"
        "        uint32 h = 4;\n"
        "        uint32 n = w * h;\n"
        "        Image2D img = heap Image2D(w, h);\n"
        "        KernelStream s #= KernelStream.current();\n"
        "        fill.launch(s, grid: [1], block: [64])(img, w, h);\n"
        "        s.sync();\n"
        "        rmw.launch(s, grid: [1], block: [64])(img, w, h);\n"
        "        s.sync();\n"
        "        float32[] out = heap float32[n];\n"
        "        for (uint32 i = 0; i < n; i = i + 1) { out[i] = -1.0f; }\n"
        "        img.download(out);\n"
        "        if (out[0] == -1.0f) { return (int32)(555); }\n"   // surface runtime absent (pre-B5)
        "        for (uint32 i = 0; i < n; i = i + 1) {\n"
        "            float32 d = out[i] - (float32)(2 * i + 1);\n"
        "            if (d < -0.01f || d > 0.01f) { return (int32)(100 + i); }\n"
        "        }\n"
        "        return 777;\n"
        "    }\n"
        "}\n";
    cajeta_test::CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Nvptx};
    auto jit = cajeta_test::CajetaJit::compile(src, "test.ImgRmwNv", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    if (r == 555) {
        GTEST_SKIP() << "CUDA storage-image surface runtime not wired yet "
                        "(pending B5); NVPTX storeImage/loadImage lowering is "
                        "emit-verified by lowersImageStoreLoadToPtxSurf";
    }
    EXPECT_EQ(r, 777) << "fail code " << r
                      << " (100+i: out[i] != 2*i+1 — storeImage/loadImage RMW)";
}

// ptxas assembles the SAXPY PTX into a cubin — proving LLVM 23's PTX is
// accepted by the CUDA 12.9 assembler for sm_89. No GPU needed (ptxas is
// a host tool); skipped if ptxas isn't installed.
TEST(XpuNvptxEmitTests, assemblesSaxpyPtxToCubin) {
    if (findPtxas().empty()) {
        GTEST_SKIP() << "ptxas not found; skipping cubin assembly";
    }

    auto src =
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.KernelThread;\n"
        "public class M {\n"
        "    @Kernel\n"
        "    public static void saxpy(KernelBuffer<float32> y, KernelBuffer<float32> x,\n"
        "                              float32 a, uint32 n) {\n"
        "        uint32 i = KernelThread.globalIdX();\n"
        "        if (i < n) { y[i] = a * x[i] + y[i]; }\n"
        "    }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.M");
    auto saxpy = findMethod(module->getStructures()["test.M"], "saxpy");
    ASSERT_NE(saxpy, nullptr);

    auto tm = createNvptxTargetMachine("sm_89");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext deviceCtx;
    llvm::Module deviceModule("xpu_saxpy_device", deviceCtx);
    configureDeviceModule(deviceModule, *tm);
    lowerKernel(saxpy, deviceModule);
    std::string ptx = emitPtx(deviceModule, *tm);
    ASSERT_FALSE(ptx.empty());

    std::vector<uint8_t> cubin = assembleCubin(ptx, "sm_89");
    ASSERT_FALSE(cubin.empty()) << "ptxas produced no cubin";
    // cubin is an ELF object: magic 0x7F 'E' 'L' 'F'.
    ASSERT_GE(cubin.size(), 4u);
    EXPECT_EQ(cubin[0], 0x7Fu);
    EXPECT_EQ(cubin[1], 'E');
    EXPECT_EQ(cubin[2], 'L');
    EXPECT_EQ(cubin[3], 'F');
}

// Stage 9: scoped memory fences lower to NVPTX membar (a memory fence with NO
// bar.sync — no thread rendezvous). Barrier.deviceMemory → membar.gl (global
// scope); Barrier.workgroupMemory → membar.cta (CTA scope). Emit-only (◐): no
// CUDA silicon here, so this proves the PTX, like the other NVPTX emit tests.
TEST(XpuNvptxEmitTests, lowersMemoryFenceToPtxMembar) {
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

    auto tm = createNvptxTargetMachine("sm_89");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext deviceCtx;
    llvm::Module deviceModule("xpu_fence_device", deviceCtx);
    configureDeviceModule(deviceModule, *tm);
    llvm::Function* fn = lowerKernel(fence, deviceModule);
    ASSERT_NE(fn, nullptr);

    std::string ir;
    { llvm::raw_string_ostream os(ir); deviceModule.print(os, nullptr); }
    EXPECT_NE(ir.find("llvm.nvvm.membar.gl"), std::string::npos) << ir;
    EXPECT_NE(ir.find("llvm.nvvm.membar.cta"), std::string::npos) << ir;

    std::string ptx = emitPtx(deviceModule, *tm);
    ASSERT_FALSE(ptx.empty());
    EXPECT_NE(ptx.find("membar.gl"), std::string::npos) << ptx;
    EXPECT_NE(ptx.find("membar.cta"), std::string::npos) << ptx;
}

// Stage 9: an explicit MemoryOrder threads through to the atomicrmw ordering on
// a backend that honours it (NVPTX uses the portable seam — no Vulkan clamp).
// MemoryOrder.Relaxed → `monotonic`, MemoryOrder.AcqRel → `acq_rel`. Proves the
// enum constant resolves in the kernel body and maps to the right ordering.
TEST(XpuNvptxEmitTests, lowersRelaxedAtomicToMonotonicPtx) {
    auto src =
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.KernelThread;\n"
        "import cajeta.xpu.MemoryOrder;\n"
        "public class MO {\n"
        "    @Kernel\n"
        "    public static void bump(KernelBuffer<int32> a, KernelBuffer<int32> b,\n"
        "                            uint32 n) {\n"
        "        uint32 i = KernelThread.globalIdX();\n"
        "        if (i < n) {\n"
        "            a.atomicAdd(0, 1, MemoryOrder.Relaxed);\n"
        "            b.atomicAdd(0, 1, MemoryOrder.AcqRel);\n"
        "        }\n"
        "    }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.MO");
    auto klass = module->getStructures()["test.MO"];
    ASSERT_NE(klass, nullptr);
    auto bump = findMethod(klass, "bump");
    ASSERT_NE(bump, nullptr);

    auto tm = createNvptxTargetMachine("sm_89");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext deviceCtx;
    llvm::Module deviceModule("xpu_memorder_device", deviceCtx);
    configureDeviceModule(deviceModule, *tm);
    ASSERT_NE(lowerKernel(bump, deviceModule), nullptr);

    std::string ir;
    { llvm::raw_string_ostream os(ir); deviceModule.print(os, nullptr); }
    EXPECT_NE(ir.find("monotonic"), std::string::npos) << ir;  // Relaxed
    EXPECT_NE(ir.find("acq_rel"), std::string::npos) << ir;    // AcqRel
}

// Stage 11: device printf on NVPTX is the external `vprintf(i8* fmt, i8* args)` —
// the format string lowers to a private constant, the args pack into a stack
// buffer. Emit-only (no CUDA hardware here): assert the IR + PTX carry vprintf.
TEST(XpuNvptxEmitTests, lowersDebugPrintfToVprintf) {
    auto src =
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.KernelThread;\n"
        "public class Prnt {\n"
        "    @Kernel\n"
        "    public static void k(KernelBuffer<int32> out, uint32 n) {\n"
        "        uint32 i = KernelThread.globalIdX();\n"
        "        if (i < n) {\n"
        "            Debug.printf(\"cajeta-pf %d\\n\", (int32) i);\n"
        "            out[i] = (int32) i;\n"
        "        }\n"
        "    }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.Prnt");
    auto klass = module->getStructures()["test.Prnt"];
    ASSERT_NE(klass, nullptr);
    auto k = findMethod(klass, "k");
    ASSERT_NE(k, nullptr);

    auto tm = createNvptxTargetMachine("sm_89");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext deviceCtx;
    llvm::Module deviceModule("xpu_printf_device", deviceCtx);
    configureDeviceModule(deviceModule, *tm);
    ASSERT_NE(lowerKernel(k, deviceModule), nullptr);

    std::string ir;
    { llvm::raw_string_ostream os(ir); deviceModule.print(os, nullptr); }
    EXPECT_NE(ir.find("vprintf"), std::string::npos) << ir;
    EXPECT_NE(ir.find("cajeta-pf"), std::string::npos) << ir;  // the format const

    std::string ptx = emitPtx(deviceModule, *tm);
    ASSERT_FALSE(ptx.empty());
    EXPECT_NE(ptx.find("vprintf"), std::string::npos) << ptx;
}

// ----- Stage 11: bounded device-side dispatch — portable lowering shape -----
//
// A device dispatch table `((int32)->int32)[] ops = { sq, cube, neg }` indexed
// by a runtime value `ops[i % 3](i)` must lower to an i32 tag-compare + if/else
// chain of DIRECT calls — NEVER a function-pointer load or indirect call (SPIR-V
// has no function pointers; this is the shape that rides all four backends). The
// IR check is the load-bearing one: every call in the device module resolves to
// a concrete Function (direct), so there is no indirect dispatch. NVPTX then
// emits valid PTX with the tag compares + branches. Always runs (no CUDA HW).
TEST(XpuNvptxEmitTests, lowersDeviceDispatchToBranchChain) {
    auto src =
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.KernelThread;\n"
        "public class Ops {\n"
        "    @Device public static int32 sq(int32 x)   { return x * x; }\n"
        "    @Device public static int32 cube(int32 x) { return x * x * x; }\n"
        "    @Device public static int32 neg(int32 x)  { return 0 - x; }\n"
        "}\n"
        "public class M {\n"
        "    @Kernel\n"
        "    public static void dispatch(KernelBuffer<int32> out, uint32 n) {\n"
        "        uint32 i = KernelThread.globalIdX();\n"
        "        if (i < n) {\n"
        "            ((int32) -> int32)[] ops = { Ops::sq, Ops::cube, Ops::neg };\n"
        "            uint32 sel = i % 3;\n"
        "            out[i] = ops[sel]((int32) i);\n"
        "        }\n"
        "    }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.M");
    auto klass = module->getStructures()["test.M"];
    ASSERT_NE(klass, nullptr);
    auto dispatch = findMethod(klass, "dispatch");
    ASSERT_NE(dispatch, nullptr);

    auto tm = createNvptxTargetMachine("sm_89");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext deviceCtx;
    llvm::Module deviceModule("xpu_dispatch_device", deviceCtx);
    configureDeviceModule(deviceModule, *tm);

    llvm::Function* fn = lowerKernel(dispatch, deviceModule);
    ASSERT_NE(fn, nullptr);

    // Portable-shape invariant: NO indirect call anywhere in the device module.
    // Every CallInst must resolve to a concrete callee (a direct call); an
    // indirect call (function-pointer dispatch) would have a null
    // getCalledFunction(). Intrinsics are direct calls and fine.
    size_t indirectCalls = 0, directCalls = 0;
    for (llvm::Function& f : deviceModule) {
        for (llvm::BasicBlock& bb : f) {
            for (llvm::Instruction& inst : bb) {
                if (auto* call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
                    if (call->getCalledFunction() == nullptr) ++indirectCalls;
                    else ++directCalls;
                }
            }
        }
    }
    EXPECT_EQ(indirectCalls, 0u)
        << "device dispatch must never emit an indirect call / fn-pointer load";

    std::string ptx = emitPtx(deviceModule, *tm);
    ASSERT_FALSE(ptx.empty());
    EXPECT_NE(ptx.find(".visible .entry dispatch"), std::string::npos) << ptx;
    // The if/else chain compares the i32 tag against the candidate indices —
    // an equality test per arm (the alwaysinline candidate bodies then fold in).
    EXPECT_NE(ptx.find("setp.eq"), std::string::npos) << ptx;
    EXPECT_NE(ptx.find("bra"), std::string::npos) << ptx;
}
