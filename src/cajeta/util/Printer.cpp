//
// Created by James Klappenbach on 11/9/22.
//

#include "Printer.h"
#include "../compile/CajetaModule.h"

namespace cajeta {
    llvm::FunctionCallee Printer::getPrintf(CajetaModulePtr module) {
        llvm::Function* fn = module->getLlvmModule()->getFunction("printf");
        llvm::FunctionCallee fnCallee;
        vector<llvm::Type*> args({llvm::Type::getInt8PtrTy(*module->getLlvmContext())});
        llvm::Type* returnType = llvm::Type::getInt32Ty(*module->getLlvmContext());
        if (!fn) {
            llvm::FunctionType* fnType = llvm::FunctionType::get(returnType, llvm::ArrayRef<llvm::Type*>(args), true);
            fnCallee = module->getLlvmModule()->getOrInsertFunction("printf", fnType);
        } else {
            fnCallee = llvm::FunctionCallee(fn->getFunctionType(), fn);
        }
        return fnCallee;
    }

//        func_printf = llvm::Function::Create(FuncTy9, llvm::GlobalValue::ExternalLinkage, "printf", pModule->getLlvmModule());
//        func_printf->setCallingConv(CallingConv::C);
//        AttrListPtr func_printf_PAL;
//        func_printf->setAttributes(func_printf_PAL);

    llvm::CallInst* Printer::createPrintfInstruction(CajetaModulePtr module, vector<llvm::Value*> printArgs,
        llvm::BasicBlock* basicBlock) {
        llvm::FunctionCallee fnCallee = getPrintf(module);
        return llvm::CallInst::Create(fnCallee,
            llvm::ArrayRef<llvm::Value*>(printArgs),
            "",
            basicBlock);
    }

}
