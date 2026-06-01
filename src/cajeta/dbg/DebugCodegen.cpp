#include "cajeta/dbg/DebugCodegen.h"

#include "llvm/IR/IRBuilder.h"

namespace cajeta::dbg {

    namespace {
        // Common guard: debug-info on, builder present, current block not yet
        // terminated. Returns the builder (ready to emit) or nullptr to skip.
        llvm::IRBuilder<>* emitGuard(const cajeta::CajetaModulePtr& module) {
            if (!module->getFlags().debugInfo) return nullptr;
            llvm::IRBuilder<>* builder = module->getBuilder();
            if (!builder) return nullptr;
            llvm::BasicBlock* bb = builder->GetInsertBlock();
            if (!bb || bb->getTerminator()) return nullptr;
            return builder;
        }
    }

    void emitDbgFrameEnter(cajeta::CajetaModulePtr module, const std::string& func) {
        llvm::IRBuilder<>* builder = emitGuard(module);
        if (!builder) return;
        llvm::Function* fn = module->getRuntimeFunction("__cajeta_dbg_frame_enter");
        if (!fn) return;
        llvm::Value* funcName = builder->CreateGlobalStringPtr(func);
        builder->CreateCall(fn, {funcName});
    }

    void emitDbgFrameLeave(cajeta::CajetaModulePtr module) {
        llvm::IRBuilder<>* builder = emitGuard(module);
        if (!builder) return;
        llvm::Function* fn = module->getRuntimeFunction("__cajeta_dbg_frame_leave");
        if (!fn) return;
        builder->CreateCall(fn, {});
    }

    void emitDbgLocal(cajeta::CajetaModulePtr module, const std::string& name,
                      const std::string& type, llvm::Value* slot,
                      MemoryFacets facets) {
        llvm::IRBuilder<>* builder = emitGuard(module);
        if (!builder || !slot) return;
        llvm::Function* fn = module->getRuntimeFunction("__cajeta_dbg_local");
        if (!fn) return;
        llvm::Value* nameC = builder->CreateGlobalStringPtr(name);
        llvm::Value* typeC = builder->CreateGlobalStringPtr(type);
        // The facet enums travel as two i8s, matching __cajeta_dbg_local's
        // (name, type, addr, alloc, ownership) ABI in cajeta_runtime.c.
        llvm::Value* allocC = builder->getInt8(static_cast<uint8_t>(facets.alloc));
        llvm::Value* ownC   = builder->getInt8(static_cast<uint8_t>(facets.ownership));
        // Opaque pointers: the alloca is already a ptr; no bitcast needed.
        builder->CreateCall(fn, {nameC, typeC, slot, allocC, ownC});
    }

} // namespace cajeta::dbg
