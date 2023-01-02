//
// Created by James Klappenbach on 11/9/22.
//

#include "Printer.h"
#include <cajeta/compile/CajetaModule.h>

namespace cajeta {
    llvm::FunctionCallee getPrintf(CajetaModule* module) {
        llvm::Function* fn = module->getLlvmModule()->getFunction("printf");
        llvm::FunctionCallee fnCallee;
        if (!fn) {
            llvm::FunctionType* fnType = llvm::FunctionType::get(llvm::IntegerType::get(module->getLlvmModule()->getContext(), 32), true);
            fnCallee = module->getLlvmModule()->getOrInsertFunction("printf", fnType);
        } else {
            fnCallee = llvm::FunctionCallee(fn->getFunctionType(), fn);
        }
        return fnCallee;
    }

//        func_printf = llvm::Function::Create(FuncTy9, llvm::GlobalValue::ExternalLinkage, "printf", module->getLlvmModule());
//        func_printf->setCallingConv(CallingConv::C);
//        AttrListPtr func_printf_PAL;
//        func_printf->setAttributes(func_printf_PAL);

    llvm::CallInst* Printer::createPrintfInstruction(CajetaModule* module, vector<llvm::Value*> printArgs,
            llvm::BasicBlock* basicBlock) {
        llvm::FunctionCallee fnCallee = getPrintf(module);
        return llvm::CallInst::Create(fnCallee,
            llvm::ArrayRef<llvm::Value*>(printArgs),
            "",
            basicBlock);
    }

}
