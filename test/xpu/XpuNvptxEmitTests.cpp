//
// CajetaXPU step 8 (increment C) — NVPTX PTX emission.
//
// First proof point: LLVM 22's in-tree NVPTX backend can lower a device
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
    // Buffer params land in global memory (opaque 32-bit loads emit as .b32).
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
        "import cajeta.xpu.core.Buffer;\n"
        "import cajeta.xpu.core.Texture2D;\n"
        "import cajeta.xpu.core.Sampler;\n"
        "import cajeta.xpu.core.Thread;\n"
        "public class M {\n"
        "    @Kernel\n"
        "    public static void sampleTex(Texture2D tex, Sampler s,\n"
        "                                 Buffer<float32> out, uint32 n) {\n"
        "        uint32 i = Thread.globalIdX();\n"
        "        if (i < n) { out[i] = tex.sample(s, 0.5, 0.5); }\n"
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

// ptxas assembles the SAXPY PTX into a cubin — proving LLVM 22's PTX is
// accepted by the CUDA 12.9 assembler for sm_89. No GPU needed (ptxas is
// a host tool); skipped if ptxas isn't installed.
TEST(XpuNvptxEmitTests, assemblesSaxpyPtxToCubin) {
    if (findPtxas().empty()) {
        GTEST_SKIP() << "ptxas not found; skipping cubin assembly";
    }

    auto src =
        "package test;\n"
        "import cajeta.xpu.core.Buffer;\n"
        "import cajeta.xpu.core.Thread;\n"
        "public class M {\n"
        "    @Kernel\n"
        "    public static void saxpy(Buffer<float32> y, Buffer<float32> x,\n"
        "                              float32 a, uint32 n) {\n"
        "        uint32 i = Thread.globalIdX();\n"
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
