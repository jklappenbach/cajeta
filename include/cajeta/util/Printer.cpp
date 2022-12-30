//
// Created by James Klappenbach on 11/9/22.
//

#include "Printer.h"
#include <cajeta/compile/CajetaModule.h>

namespace cajeta {
    std::mutex Printer::mutex;
    Printer* Printer::theInstance = nullptr;

    Printer::Printer(CajetaModule* module) {
        init(module);
    }

    Printer* Printer::getInstance(CajetaModule* module) {
        mutex.lock();
        if (!theInstance) {
            theInstance = new Printer(module);
        }
        mutex.unlock();
        return theInstance;
    }

//        func_printf = llvm::Function::Create(FuncTy9, llvm::GlobalValue::ExternalLinkage, "printf", module->getLlvmModule());
//        func_printf->setCallingConv(CallingConv::C);
//        AttrListPtr func_printf_PAL;
//        func_printf->setAttributes(func_printf_PAL);

    void Printer::init(CajetaModule* module) {
        printerFunctionType = llvm::FunctionType::get(llvm::IntegerType::get(module->getLlvmModule()->getContext(), 32), true);
        printerFunctionCallee = module->getLlvmModule()->getOrInsertFunction("printf", printerFunctionType);
    }

    llvm::CallInst* Printer::createPrintfInstruction(llvm::ConstantDataArray* format,
            vector<llvm::ConstantDataArray*> printArgs, llvm::BasicBlock* basicBlock) {
        vector<llvm::Value*> args;
        args.push_back(format);
        args.insert(args.begin(), printArgs.begin(), printArgs.end());
        return llvm::CallInst::Create(printerFunctionCallee,
            llvm::ArrayRef<llvm::Value*>(args),
            "",
            basicBlock);
    }

    llvm::CallInst* Printer::createPrintfInstruction(llvm::Value* format, vector<llvm::Value*> printArgs,
            llvm::BasicBlock* basicBlock) {
        vector<llvm::Value*> args;
        args.push_back(format);
        args.insert(args.begin(), printArgs.begin(), printArgs.end());
        return llvm::CallInst::Create(printerFunctionCallee,
            llvm::ArrayRef<llvm::Value*>(args),
            "",
            basicBlock);
    }

}
