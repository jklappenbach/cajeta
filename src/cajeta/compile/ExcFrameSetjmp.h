#pragma once
//
// The one place that emits the exception-frame capture call for cajeta's
// setjmp/longjmp-based try/catch (TryStatement, after-throwing advice, and
// the spawn trampoline all route here).
//
// On ELF/MachO this is a plain `setjmp(frame)` — libc's capture, and longjmp
// is a register restore. On COFF x86-64 it must NOT be: MSVCRT's longjmp
// performs a full SEH unwind (RtlUnwindEx) through every frame between the
// longjmp and the capture whenever the jmp_buf's Frame slot is non-NULL —
// and mingw's <setjmp.h> macro captures with a live frame pointer. Cajeta's
// throw path longjmps across JIT'd frames whose unwind tables the JIT
// deliberately drops (JitCoffLinking.h dropSehFrames: their 32-bit-RVA
// .pdata cannot relocate against a far JIT slab), so that unwind walks
// unregistered frames and kills the process — found as
// KernelProtocolTests.throwingCellRepliesError dying with a bare exit 127 on
// the Windows JIT. Capturing via `_setjmp(frame, NULL)` zeroes the Frame
// slot, which is the documented MSVCRT opt-out: longjmp then restores
// registers without unwinding — the exact semantics the other platforms
// already have, and the runtime's own guard captures
// (cajeta_rt_session.c) use the same form for the same reason.
//
// The frame pointer passed here is the runtime's cajeta_exception_frame,
// whose jmp_buf is its FIRST field — the capture writes the buffer through
// the frame pointer directly. Callers must give the frame alloca 16-byte
// alignment: MSVCRT's _setjmp stores XMM registers into the _JUMP_BUFFER
// with aligned stores.
//
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/TargetParser/Triple.h>

namespace cajeta {

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
