#pragma once
// The one place that emits the exception-frame capture for cajeta's
// setjmp/longjmp try/catch. Callers must give the frame alloca 16-byte
// alignment: MSVCRT's _setjmp stores XMM registers with aligned stores.
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/TargetParser/Triple.h>

namespace cajeta {

// Capture into the runtime's cajeta_exception_frame, whose jmp_buf is its first
// field. On COFF the form MUST be `_setjmp(frame, NULL)`: a non-NULL Frame slot
// makes longjmp SEH-unwind through JIT frames whose unwind tables were dropped.
inline llvm::CallInst* emitExcFrameSetjmp(llvm::IRBuilder<>& builder,
                                          llvm::Value* framePtr) {
    llvm::Module* lmod = builder.GetInsertBlock()->getModule();
    llvm::LLVMContext& ctx = lmod->getContext();
    llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
    llvm::PointerType* ptrTy = llvm::PointerType::get(ctx, 0);
    const llvm::Triple triple(lmod->getTargetTriple());

    if (triple.isOSBinFormatCOFF()) {
        llvm::Function* fn = lmod->getFunction("_setjmp");
        if (!fn) {
            llvm::FunctionType* ty =
                llvm::FunctionType::get(i32Ty, {ptrTy, ptrTy}, false);
            fn = llvm::Function::Create(ty, llvm::Function::ExternalLinkage,
                                        "_setjmp", lmod);
            fn->addFnAttr(llvm::Attribute::ReturnsTwice);
        }
        return builder.CreateCall(
            fn, {framePtr, llvm::ConstantPointerNull::get(ptrTy)});
    }

    llvm::Function* fn = lmod->getFunction("setjmp");
    if (!fn) {
        llvm::FunctionType* ty = llvm::FunctionType::get(i32Ty, {ptrTy}, false);
        fn = llvm::Function::Create(ty, llvm::Function::ExternalLinkage,
                                    "setjmp", lmod);
        fn->addFnAttr(llvm::Attribute::ReturnsTwice);
    }
    return builder.CreateCall(fn, {framePtr});
}

} // namespace cajeta
