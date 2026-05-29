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

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/IntrinsicsNVPTX.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Target/TargetMachine.h"

#include <string>

using namespace cajeta::xpu::nvidia;

namespace {

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
