#include "cajeta/dbg/LineInfoCodegen.h"

#include "llvm/IR/IRBuilder.h"

namespace cajeta::dbg {

    namespace {
        // Guard: line-info on, builder present, current block not terminated.
        llvm::IRBuilder<>* lineGuard(const cajeta::CajetaModulePtr& module) {
            if (!module->getFlags().lineInfo) return nullptr;
            llvm::IRBuilder<>* builder = module->getBuilder();
            if (!builder) return nullptr;
            llvm::BasicBlock* bb = builder->GetInsertBlock();
            if (!bb || bb->hasTerminator()) return nullptr;
            return builder;
        }
    }

    void emitLineEnter(cajeta::CajetaModulePtr module, const std::string& typeName,
                       const std::string& methodName, const std::string& fileName) {
        llvm::IRBuilder<>* builder = lineGuard(module);
        if (!builder) return;
        llvm::Function* fn = module->getRuntimeFunction("__cajeta_line_enter");
        if (!fn) return;
        auto& ctx = *module->getLlvmContext();
        llvm::Type* ptrTy = llvm::PointerType::get(ctx, 0);
        // #FrameDesc { i8* typeName, i8* methodName, i8* fileName } — one private
        // constant per method, program lifetime; matches CajetaFrameDesc in
        // cajeta_rt_core.c. The strings land in the builder's current module.
        llvm::Constant* tC = builder->CreateGlobalString(typeName);
        llvm::Constant* mC = builder->CreateGlobalString(methodName);
        llvm::Constant* fC = builder->CreateGlobalString(fileName);
        llvm::StructType* descTy = llvm::StructType::get(ctx, {ptrTy, ptrTy, ptrTy});
        llvm::Module* mod = builder->GetInsertBlock()->getModule();
        auto* desc = new llvm::GlobalVariable(
            *mod, descTy, /*isConstant=*/true, llvm::GlobalValue::PrivateLinkage,
            llvm::ConstantStruct::get(descTy, {tC, mC, fC}), ".cajeta.framedesc");
        builder->CreateCall(fn, {desc});
    }

    void emitLineLeave(cajeta::CajetaModulePtr module) {
        llvm::IRBuilder<>* builder = lineGuard(module);
        if (!builder) return;
        llvm::Function* fn = module->getRuntimeFunction("__cajeta_line_leave");
        if (!fn) return;
        builder->CreateCall(fn, {});
    }

    void emitLineMark(cajeta::CajetaModulePtr module, int line) {
        llvm::IRBuilder<>* builder = lineGuard(module);
        if (!builder || line <= 0) return;
        llvm::Function* fn = module->getRuntimeFunction("__cajeta_line_mark");
        if (!fn) return;
        builder->CreateCall(fn, {builder->getInt32(line)});
    }

} // namespace cajeta::dbg
