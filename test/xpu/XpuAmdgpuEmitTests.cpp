//
// CajetaXPU AMD bring-up (cajeta-amd.md Increment 1) — AMDGPU ISA emission.
//
// First AMD proof point: LLVM 22's in-tree AMDGPU backend can lower a device
// llvm::Module to AMDGCN ISA text in THIS build, and (if ld.lld is present)
// link the relocatable object into an hsaco code object. This isolates the
// toolchain risk (does AMDGPU emit at all?) from the later kernel-body
// lowering risk (Increment 2) — so it hand-builds a minimal kernel module
// rather than going through Cajeta AST lowering.
//
// No GPU required: ISA is text; lld is a host tool. The on-device launch is
// XpuSaxpyAmdDeviceTests (Increment 4).
//

#include "gtest/gtest.h"

#include "cajeta/xpu/amd/AmdgpuBackend.h"
#include "XpuDeviceTestUtil.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Target/TargetMachine.h"

#include <string>

using namespace cajeta::xpu::amd;

namespace {

// Hand-build a minimal AMDGPU kernel:
//   amdgpu_kernel void probe(ptr addrspace(1) out) {
//     out[workitem.id.x] = 1.0f;
//   }
// AMDGPU marks kernels purely by calling convention (amdgpu_kernel) — there
// is no nvvm.annotations-style metadata, which is itself a seam data point.
std::string buildProbeKernel(llvm::Module& m, llvm::LLVMContext& ctx) {
    auto* f32 = llvm::Type::getFloatTy(ctx);
    auto* i64 = llvm::Type::getInt64Ty(ctx);
    auto* voidTy = llvm::Type::getVoidTy(ctx);
    auto* globalPtr = llvm::PointerType::get(ctx, /*addrspace=*/1);

    auto* fnTy = llvm::FunctionType::get(voidTy, {globalPtr}, /*vararg=*/false);
    auto* fn = llvm::Function::Create(
        fnTy, llvm::Function::ExternalLinkage, "probe", &m);
    fn->setCallingConv(llvm::CallingConv::AMDGPU_KERNEL);

    llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx, "entry", fn));
    llvm::Function* tidX = llvm::Intrinsic::getOrInsertDeclaration(
        &m, llvm::Intrinsic::amdgcn_workitem_id_x);
    llvm::Value* tid = b.CreateCall(tidX, {}, "tid");
    llvm::Value* idx = b.CreateZExt(tid, i64);
    llvm::Value* gep = b.CreateGEP(f32, fn->getArg(0), {idx}, "p");
    b.CreateStore(llvm::ConstantFP::get(f32, 1.0), gep);
    b.CreateRetVoid();

    return "probe";
}

} // namespace

// The amdgcn target is registered in this LLVM build and yields a usable
// TargetMachine for gfx1151 (Strix Halo / RDNA 3.5).
TEST(XpuAmdgpuEmitTests, amdgpuTargetMachineAvailable) {
    auto tm = createAmdgpuTargetMachine("gfx1151");
    ASSERT_NE(tm, nullptr) << "amdgcn target not built into this LLVM";
}

// A hand-built device kernel lowers to AMDGCN ISA containing the kernel
// label, the HSA kernel descriptor, the arch directive, a global store, and
// the end-of-program marker.
TEST(XpuAmdgpuEmitTests, emitsIsaForHandBuiltKernel) {
    auto tm = createAmdgpuTargetMachine("gfx1151");
    ASSERT_NE(tm, nullptr);

    llvm::LLVMContext ctx;
    llvm::Module m("xpu_amdgpu_probe", ctx);
    configureDeviceModule(m, *tm);
    std::string name = buildProbeKernel(m, ctx);

    std::string isa = emitIsa(m, *tm);
    ASSERT_FALSE(isa.empty()) << "AMDGPU emitted no ISA";

    // The kernel symbol + the HSA kernel descriptor the loader keys on.
    EXPECT_NE(isa.find(name), std::string::npos) << isa;
    EXPECT_NE(isa.find(".amdhsa_kernel probe"), std::string::npos) << isa;
    // The arch we asked for shows up in the target directive.
    EXPECT_NE(isa.find("gfx1151"), std::string::npos) << isa;
    // A global-memory store (gfx11 uses global_store_*) and the end marker.
    EXPECT_NE(isa.find("global_store"), std::string::npos) << isa;
    EXPECT_NE(isa.find("s_endpgm"), std::string::npos) << isa;
}

// ld.lld links the relocatable AMDGCN object into an hsaco code object —
// proving LLVM 22's AMDGPU object is accepted by lld for gfx1151. Gated on an
// actual ROCm/HIP device: the link needs ROCm's lld (a generic host ld.lld on
// PATH cannot link an amdgpu object — it errors "incompatible with
// elf64-x86-64"), so hardware presence is the reliable skip condition.
TEST(XpuAmdgpuEmitTests, assemblesProbeIsaToHsaco) {
    CAJETA_SKIP_IF_NO_HIP();
    auto tm = createAmdgpuTargetMachine("gfx1151");
    ASSERT_NE(tm, nullptr);

    llvm::LLVMContext ctx;
    llvm::Module m("xpu_amdgpu_probe", ctx);
    configureDeviceModule(m, *tm);
    buildProbeKernel(m, ctx);

    std::vector<uint8_t> hsaco = assembleHsaco(m, *tm, "gfx1151");
    ASSERT_FALSE(hsaco.empty()) << "lld produced no hsaco";
    // hsaco is an ELF code object: magic 0x7F 'E' 'L' 'F'.
    ASSERT_GE(hsaco.size(), 4u);
    EXPECT_EQ(hsaco[0], 0x7Fu);
    EXPECT_EQ(hsaco[1], 'E');
    EXPECT_EQ(hsaco[2], 'L');
    EXPECT_EQ(hsaco[3], 'F');
}
