#include "cajeta/dbg/DebugCodegen.h"
#include "cajeta/dbg/DebugLocTable.h"
#include "cajeta/dbg/DebugTypeTable.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

#include <map>

namespace cajeta::dbg {

    namespace {
        llvm::IRBuilder<>* emitGuard(const cajeta::CajetaModulePtr& module) {
            if (!module->getFlags().debugInfo) return nullptr;
            llvm::IRBuilder<>* builder = module->getBuilder();
            if (!builder) return nullptr;
            llvm::BasicBlock* bb = builder->GetInsertBlock();
            if (!bb || bb->hasTerminator()) return nullptr;
            return builder;
        }
    }

    llvm::Value* emitDbgFrameEnter(cajeta::CajetaModulePtr module,
                                   const std::string& func) {
        llvm::IRBuilder<>* builder = emitGuard(module);
        if (!builder) return nullptr;
        llvm::Function* fn = module->getRuntimeFunction("__cajeta_dbg_frame_enter");
        if (!fn) return nullptr;
        llvm::Value* funcName = builder->CreateGlobalString(func);
        return builder->CreateCall(fn, {funcName});
    }

    void emitDbgFrameLeave(cajeta::CajetaModulePtr module,
                           llvm::Value* nodeSlot) {
        llvm::IRBuilder<>* builder = emitGuard(module);
        if (!builder || !nodeSlot) return;
        llvm::Function* fn = module->getRuntimeFunction("__cajeta_dbg_frame_leave");
        if (!fn) return;
        llvm::Value* node = builder->CreateLoad(
            llvm::PointerType::get(builder->getContext(), 0), nodeSlot,
            "__dbg_frame_node");
        builder->CreateCall(fn, {node});
    }

    void emitDbgLocal(cajeta::CajetaModulePtr module, const std::string& name,
                      const std::string& type, llvm::Value* slot,
                      MemoryFacets facets, llvm::Value* dropEntry) {
        llvm::IRBuilder<>* builder = emitGuard(module);
        if (!builder || !slot) return;
        llvm::Function* fn = module->getRuntimeFunction("__cajeta_dbg_local");
        if (!fn) return;
        // A debug-type root: on a cache hit the type world that answers it is gone.
        globalDebugTypeTable().addRoot(type);
        llvm::Value* nameC = builder->CreateGlobalString(name);
        llvm::Value* typeC = builder->CreateGlobalString(type);
        // __cajeta_dbg_local's ABI in cajeta_runtime.c is
        // (name, type, addr, alloc, ownership, drop_entry): two i8s, then a ptr.
        llvm::Value* allocC = builder->getInt8(static_cast<uint8_t>(facets.alloc));
        llvm::Value* ownC   = builder->getInt8(static_cast<uint8_t>(facets.ownership));
        llvm::Value* dropC = dropEntry
            ? dropEntry
            : llvm::ConstantPointerNull::get(
                  llvm::PointerType::get(*module->getLlvmContext(), 0));
        builder->CreateCall(fn, {nameC, typeC, slot, allocC, ownC, dropC});
    }

    void emitDbgLocTable(cajeta::CajetaModulePtr module) {
        if (!module || !module->getFlags().debugInfo) return;
        const DbgLocTable& table = globalDbgLocTable();
        if (table.empty()) return;

        llvm::Module* lmod = module->getLlvmModule();
        if (!lmod) return;
        // Idempotent: a second table would bring a second ctor, and the last wins.
        if (lmod->getNamedGlobal("__cajeta.dbg.loctable")) return;

        llvm::Function* regFn =
            module->getRuntimeFunction("__cajeta_dbg_register_loc_table");
        if (!regFn) return;

        auto& ctx = *module->getLlvmContext();
        llvm::Type* ptrTy = llvm::PointerType::get(ctx, 0);
        llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
        // Matches CajetaDbgLocEntry in cajeta_rt_core.c.
        llvm::StructType* entryTy =
            llvm::StructType::get(ctx, {ptrTy, i32Ty, i32Ty, ptrTy});

        std::map<std::string, llvm::Constant*> interned;
        auto str = [&](const std::string& s) -> llvm::Constant* {
            auto it = interned.find(s);
            if (it != interned.end()) return it->second;
            auto* gv = new llvm::GlobalVariable(
                *lmod, llvm::ArrayType::get(llvm::Type::getInt8Ty(ctx),
                                            s.size() + 1),
                /*isConstant=*/true, llvm::GlobalValue::PrivateLinkage,
                llvm::ConstantDataArray::getString(ctx, s),
                ".cajeta.dbg.str");
            gv->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
            interned[s] = gv;
            return gv;
        };

        std::vector<llvm::Constant*> entries;
        entries.reserve(table.size());
        for (size_t i = 0; i < table.size(); ++i) {
            const DbgLoc& loc = table.at(static_cast<int32_t>(i));
            entries.push_back(llvm::ConstantStruct::get(
                entryTy,
                {str(loc.file),
                 llvm::ConstantInt::get(i32Ty, (uint64_t) loc.line),
                 llvm::ConstantInt::get(i32Ty, (uint64_t) loc.col),
                 str(loc.function)}));
        }

        llvm::ArrayType* arrTy = llvm::ArrayType::get(entryTy, entries.size());
        auto* tableGV = new llvm::GlobalVariable(
            *lmod, arrTy, /*isConstant=*/true,
            llvm::GlobalValue::PrivateLinkage,
            llvm::ConstantArray::get(arrTy, entries), "__cajeta.dbg.loctable");

        llvm::FunctionType* ctorTy =
            llvm::FunctionType::get(llvm::Type::getVoidTy(ctx), false);
        llvm::Function* ctor = llvm::Function::Create(
            ctorTy, llvm::GlobalValue::InternalLinkage,
            "__cajeta.dbg.loctable.register", lmod);
        llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx, "entry", ctor));
        b.CreateCall(regFn, {tableGV,
                             llvm::ConstantInt::get(
                                 i32Ty, (uint64_t) entries.size())});
        b.CreateRetVoid();
        llvm::appendToGlobalCtors(*lmod, ctor, /*priority=*/65535);
    }

} // namespace cajeta::dbg
