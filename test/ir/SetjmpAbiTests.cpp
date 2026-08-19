//
// Exception-frame capture ABI (ExcFrameSetjmp.h) — the sjlj capture cajeta's
// try/catch emits must be per-object-format:
//
//   * ELF/MachO: `setjmp(frame)` — libc capture, non-unwinding longjmp.
//   * COFF: `_setjmp(frame, NULL)` — the NULL frame is load-bearing. MSVCRT's
//     longjmp SEH-unwinds (RtlUnwindEx) through every intervening frame when
//     the captured Frame slot is non-NULL, and cajeta throws cross JIT'd
//     frames whose unwind tables the COFF JIT drops (JitCoffLinking.h), so an
//     unwinding longjmp kills the process. Pinned by
//     KernelProtocolTests.throwingCellRepliesError's exit-127 death on the
//     Windows JIT; these tests keep the emitted shape honest on every host.
//
#include <gtest/gtest.h>

#include "cajeta/compile/ExcFrameSetjmp.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

namespace {

struct EmitResult {
    llvm::CallInst* call;
    std::unique_ptr<llvm::LLVMContext> ctx;
    std::unique_ptr<llvm::Module> mod;
};

EmitResult emitForTriple(const char* triple) {
    EmitResult r;
    r.ctx = std::make_unique<llvm::LLVMContext>();
    r.mod = std::make_unique<llvm::Module>("sjlj_abi_test", *r.ctx);
    // LLVM 21 narrowed setTargetTriple to llvm::Triple; 18/20 take a string
    // (same preprocess-time branch CajetaModule.cpp uses).
#if LLVM_VERSION_MAJOR >= 21
    r.mod->setTargetTriple(llvm::Triple(triple));
#else
    r.mod->setTargetTriple(triple);
#endif

    auto* i32Ty = llvm::Type::getInt32Ty(*r.ctx);
    auto* fnTy = llvm::FunctionType::get(i32Ty, false);
    auto* fn = llvm::Function::Create(fnTy, llvm::Function::ExternalLinkage,
                                      "host", r.mod.get());
    auto* bb = llvm::BasicBlock::Create(*r.ctx, "entry", fn);
    llvm::IRBuilder<> b(bb);
    auto* frame = b.CreateAlloca(
        llvm::ArrayType::get(llvm::Type::getInt8Ty(*r.ctx), 512));
    frame->setAlignment(llvm::Align(16));
    r.call = cajeta::emitExcFrameSetjmp(b, frame);
    b.CreateRet(r.call);
    return r;
}

} // namespace

TEST(SetjmpAbi, coffCapturesWithNullFrame) {
    EmitResult r = emitForTriple("x86_64-w64-windows-gnu");
    ASSERT_NE(nullptr, r.call);
    llvm::Function* callee = r.call->getCalledFunction();
    ASSERT_NE(nullptr, callee);
    EXPECT_EQ("_setjmp", callee->getName());
    ASSERT_EQ(2u, r.call->arg_size());
    // The NULL second argument is what makes MSVCRT's longjmp skip the SEH
    // unwind; a frame-carrying capture here is the Windows-JIT process kill.
    EXPECT_TRUE(llvm::isa<llvm::ConstantPointerNull>(r.call->getArgOperand(1)));
    EXPECT_TRUE(callee->hasFnAttribute(llvm::Attribute::ReturnsTwice));
}

TEST(SetjmpAbi, elfCapturesPlainSetjmp) {
    EmitResult r = emitForTriple("x86_64-unknown-linux-gnu");
    ASSERT_NE(nullptr, r.call);
    llvm::Function* callee = r.call->getCalledFunction();
    ASSERT_NE(nullptr, callee);
    EXPECT_EQ("setjmp", callee->getName());
    EXPECT_EQ(1u, r.call->arg_size());
    EXPECT_TRUE(callee->hasFnAttribute(llvm::Attribute::ReturnsTwice));
}

TEST(SetjmpAbi, machoCapturesPlainSetjmp) {
    EmitResult r = emitForTriple("arm64-apple-macosx");
    ASSERT_NE(nullptr, r.call);
    llvm::Function* callee = r.call->getCalledFunction();
    ASSERT_NE(nullptr, callee);
    EXPECT_EQ("setjmp", callee->getName());
    EXPECT_EQ(1u, r.call->arg_size());
}

// Two captures in one module share one declaration — the second emit must
// reuse the existing function, not mint a duplicate ("_setjmp.1" would link
// to nothing).
TEST(SetjmpAbi, coffReusesOneDeclaration) {
    EmitResult r = emitForTriple("x86_64-w64-windows-gnu");
    llvm::IRBuilder<> b(r.call->getParent());
    b.SetInsertPoint(r.call->getNextNode());
    auto* frame2 = b.CreateAlloca(
        llvm::ArrayType::get(llvm::Type::getInt8Ty(*r.ctx), 512));
    llvm::CallInst* second = cajeta::emitExcFrameSetjmp(b, frame2);
    EXPECT_EQ(r.call->getCalledFunction(), second->getCalledFunction());
}
