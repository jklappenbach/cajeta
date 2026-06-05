//
// S1b of the value-type operator-overloading mechanism
// (plans/value-type-overloading-plan.md, review fix #1): `alwaysinline` is an
// attribute, not a transform — it only fires when an AlwaysInlinerPass runs.
// optimizeModule's O0 path used to early-return (no passes), so alwaysinline was
// a silent no-op at O0/JIT, which would defeat the @ValueType operator
// force-inline (and spill aggregates). These tests assert optimizeModule actually
// folds an alwaysinline call at O0.
//

#include <gtest/gtest.h>

#include "cajeta/compile/Optimizer.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

#include <memory>

namespace {

// Build { i32 callee() alwaysinline { ret 42 }, i32 caller() { ret callee() } }.
std::unique_ptr<llvm::Module> buildCallerCallee(llvm::LLVMContext& ctx) {
    auto m = std::make_unique<llvm::Module>("s1b", ctx);
    auto* i32 = llvm::Type::getInt32Ty(ctx);
    auto* fnTy = llvm::FunctionType::get(i32, /*isVarArg=*/false);

    auto* callee = llvm::Function::Create(
        fnTy, llvm::Function::InternalLinkage, "callee", m.get());
    callee->addFnAttr(llvm::Attribute::AlwaysInline);
    {
        llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx, "entry", callee));
        b.CreateRet(b.getInt32(42));
    }

    auto* caller = llvm::Function::Create(
        fnTy, llvm::Function::ExternalLinkage, "caller", m.get());
    {
        llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx, "entry", caller));
        b.CreateRet(b.CreateCall(callee));
    }
    return m;
}

int callCount(llvm::Function* f) {
    int n = 0;
    for (auto& inst : llvm::instructions(*f)) {
        if (llvm::isa<llvm::CallInst>(&inst)) ++n;
    }
    return n;
}

} // namespace

// At O0 the alwaysinline callee is still folded into the caller (no residual
// call) — the AlwaysInlinerPass runs even though no optimization level is set.
TEST(OptimizerInlineTests, alwaysInlineFoldsAtO0) {
    llvm::LLVMContext ctx;
    auto m = buildCallerCallee(ctx);
    auto* caller = m->getFunction("caller");
    ASSERT_NE(caller, nullptr);
    ASSERT_EQ(callCount(caller), 1);

    cajeta::optimizeModule(*m, /*tm=*/nullptr, cajeta::OptLevel::O0);

    EXPECT_EQ(callCount(caller), 0);
}
