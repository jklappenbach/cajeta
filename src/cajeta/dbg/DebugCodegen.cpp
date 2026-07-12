#include "cajeta/dbg/DebugCodegen.h"
#include "cajeta/dbg/DebugLocTable.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

#include <map>

namespace cajeta::dbg {

    namespace {
        // Common guard: debug-info on, builder present, current block not yet
        // terminated. Returns the builder (ready to emit) or nullptr to skip.
        llvm::IRBuilder<>* emitGuard(const cajeta::CajetaModulePtr& module) {
            if (!module->getFlags().debugInfo) return nullptr;
            llvm::IRBuilder<>* builder = module->getBuilder();
            if (!builder) return nullptr;
            llvm::BasicBlock* bb = builder->GetInsertBlock();
            if (!bb || bb->hasTerminator()) return nullptr;
            return builder;
        }
    }

    void emitDbgFrameEnter(cajeta::CajetaModulePtr module, const std::string& func) {
        llvm::IRBuilder<>* builder = emitGuard(module);
        if (!builder) return;
        llvm::Function* fn = module->getRuntimeFunction("__cajeta_dbg_frame_enter");
        if (!fn) return;
        llvm::Value* funcName = builder->CreateGlobalString(func);
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
                      MemoryFacets facets, llvm::Value* dropEntry) {
        llvm::IRBuilder<>* builder = emitGuard(module);
        if (!builder || !slot) return;
        llvm::Function* fn = module->getRuntimeFunction("__cajeta_dbg_local");
        if (!fn) return;
        llvm::Value* nameC = builder->CreateGlobalString(name);
        llvm::Value* typeC = builder->CreateGlobalString(type);
        // The facet enums travel as two i8s, the drop entry as a ptr, matching
        // __cajeta_dbg_local's (name, type, addr, alloc, ownership, drop_entry)
        // ABI in cajeta_runtime.c.
        llvm::Value* allocC = builder->getInt8(static_cast<uint8_t>(facets.alloc));
        llvm::Value* ownC   = builder->getInt8(static_cast<uint8_t>(facets.ownership));
        // Non-owners have no drop entry; pass an explicit null ptr.
        llvm::Value* dropC = dropEntry
            ? dropEntry
            : llvm::ConstantPointerNull::get(
                  llvm::PointerType::get(*module->getLlvmContext(), 0));
        // Opaque pointers: the alloca is already a ptr; no bitcast needed.
        builder->CreateCall(fn, {nameC, typeC, slot, allocC, ownC, dropC});
    }

    void emitDbgLocTable(cajeta::CajetaModulePtr module) {
        if (!module || !module->getFlags().debugInfo) return;
        const DbgLocTable& table = globalDbgLocTable();
        if (table.empty()) return;

        llvm::Module* lmod = module->getLlvmModule();
        if (!lmod) return;
        // Idempotent: a second call would emit a second table and a second ctor,
        // and the last ctor to run would win.
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

        // Intern the strings: a file name repeats across every statement in the
        // file, and a function name across every statement in the method.
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

        // Register at module-init (LLJIT: initialize(); native: startup), the
        // same shape the XPU kernel registry uses. Priority 65535 keeps it after
        // any ctor that might want to run first; nothing reads the table during
        // startup.
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
