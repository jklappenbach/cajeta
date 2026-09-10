#include "cajeta/dbg/LineInfoCodegen.h"

#include "cajeta/method/Method.h"
#include "cajeta/type/CajetaClass.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

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

        // Tells the runtime, once per module, that line probes were compiled in, so
        // the profiler can refuse to arm on a --line-info=off binary. A ctor rather
        // than a weak extern, which the linked-in runtime bitcode would collide with.
        void ensureLineInfoRegistered(llvm::Module* mod,
                                      const cajeta::CajetaModulePtr& module) {
            static const char* kCtorName = "__cajeta.lineinfo.register";
            if (!mod || mod->getFunction(kCtorName)) return;
            llvm::Function* regFn =
                module->getRuntimeFunction("__cajeta_line_info_register");
            if (!regFn) return;
            auto& ctx = mod->getContext();
            llvm::FunctionType* ctorTy =
                llvm::FunctionType::get(llvm::Type::getVoidTy(ctx), false);
            llvm::Function* ctor = llvm::Function::Create(
                ctorTy, llvm::GlobalValue::InternalLinkage, kCtorName, mod);
            llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx, "entry", ctor));
            b.CreateCall(regFn, {});
            b.CreateRetVoid();
            llvm::appendToGlobalCtors(*mod, ctor, /*priority=*/65535);
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
        // { i8* typeName, i8* methodName, i8* fileName }, matching CajetaFrameDesc
        // in cajeta_rt_core.c: one private constant per method, program lifetime.
        llvm::Constant* tC = builder->CreateGlobalString(typeName);
        llvm::Constant* mC = builder->CreateGlobalString(methodName);
        llvm::Constant* fC = builder->CreateGlobalString(fileName);
        llvm::StructType* descTy = llvm::StructType::get(ctx, {ptrTy, ptrTy, ptrTy});
        llvm::Module* mod = builder->GetInsertBlock()->getModule();
        ensureLineInfoRegistered(mod, module);
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
        // A per-statement mark costs 3.5-9.4x an uninstrumented build — an opaque
        // call at every statement boundary forbids inlining — so it is emitted only
        // under debugInfo OR safepoints, either of which already accepts that cost.
        const auto& f = module->getFlags();
        if (!f.debugInfo && !f.safepoints) return;
        llvm::IRBuilder<>* builder = lineGuard(module);
        if (!builder || line <= 0) return;
        llvm::Function* fn = module->getRuntimeFunction("__cajeta_line_mark");
        if (!fn) return;
        builder->CreateCall(fn, {builder->getInt32(line)});
    }

    int fileLineFor(const cajeta::CajetaModulePtr& module, int snippetLine) {
        // The two deltas COMPOSE, not alternate: a generic method on a generic class
        // is re-parsed twice, so the method's delta maps method-snippet to class-
        // snippet and the class's maps class-snippet to file. Either alone is short.
        int delta = 0;
        if (auto method = module->getCurrentMethod()) {
            delta += method->getDbgLineDelta();
            if (auto owner = method->getParent())
                delta += owner->getDbgLineDelta();
        }
        int line = snippetLine + delta;
        return line < 1 ? 1 : line;
    }

} // namespace cajeta::dbg
