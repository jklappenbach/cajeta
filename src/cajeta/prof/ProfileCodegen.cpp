#include "cajeta/prof/ProfileCodegen.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

namespace cajeta::prof {

    namespace {
        // #ProfMethod — the LLVM mirror of CajetaProfMethod in cajeta_rt_prof_instr.c,
        // mutable and zero past the strings, so registration needs no ctor.
        llvm::StructType* profMethodTy(llvm::LLVMContext& ctx) {
            llvm::Type* ptrTy = llvm::PointerType::get(ctx, 0);
            llvm::Type* i64 = llvm::Type::getInt64Ty(ctx);
            llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
            return llvm::StructType::get(
                ctx, {ptrTy, ptrTy, ptrTy, i64, i64, i64, i32, i32, ptrTy});
        }

        llvm::IRBuilder<>* profGuard(const cajeta::CajetaModulePtr& module) {
            if (module->getFlags().profiler != Profiler::Instrument) return nullptr;
            llvm::IRBuilder<>* builder = module->getBuilder();
            if (!builder) return nullptr;
            llvm::BasicBlock* bb = builder->GetInsertBlock();
            if (!bb || bb->hasTerminator()) return nullptr;
            return builder;
        }

        // A global ctor, not a weak extern: the runtime bitcode is linked in long
        // before codegen emits its definition, so the two would collide.
        void ensureInstrRegistered(llvm::Module* mod,
                                   const cajeta::CajetaModulePtr& module) {
            static const char* kCtorName = "__cajeta.profinstr.register";
            if (!mod || mod->getFunction(kCtorName)) return;
            llvm::Function* regFn =
                module->getRuntimeFunction("__cajeta_prof_instr_register_build");
            if (!regFn) return;
            auto& ctx = mod->getContext();
            llvm::FunctionType* ctorTy =
                llvm::FunctionType::get(llvm::Type::getVoidTy(ctx), false);
            llvm::Function* ctor = llvm::Function::Create(
                ctorTy, llvm::GlobalValue::InternalLinkage, kCtorName, mod);
            llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx, "entry", ctor));
            const auto& f = module->getFlags();
            const int optLevel = f.opt == OptLevel::O0 ? 0
                               : f.opt == OptLevel::O1 ? 1
                               : f.opt == OptLevel::O2 ? 2 : 3;
            b.CreateCall(regFn, {b.CreateGlobalString(selectionFor(module).describe()),
                                 b.getInt32(optLevel)});
            b.CreateRetVoid();
            llvm::appendToGlobalCtors(*mod, ctor, /*priority=*/65535);
        }
    }

    const ProfileSelection& selectionFor(const cajeta::CajetaModulePtr& module) {
        // Memoized per thread: re-parsing would put a file parse on the emission path.
        static thread_local std::string memoText;
        static thread_local bool memoValid = false;
        static thread_local ProfileSelection memoSel;
        const std::string& text = module->getFlags().profilerSelect;
        if (!memoValid || memoText != text) {
            memoSel = ProfileSelection::parse(text);
            memoText = text;
            memoValid = true;
        }
        return memoSel;
    }

    bool isInstrumented(const cajeta::CajetaModulePtr& module,
                        const std::string& canonicalClassName) {
        if (module->getFlags().profiler != Profiler::Instrument) return false;
        return selectionFor(module).selects(canonicalClassName);
    }

    ProfileFrame emitProfileEnter(cajeta::CajetaModulePtr module,
                                  const std::string& typeName,
                                  const std::string& methodName,
                                  const std::string& fileName) {
        ProfileFrame frame;
        llvm::IRBuilder<>* builder = profGuard(module);
        if (!builder) return frame;
        if (!selectionFor(module).selects(typeName)) return frame;
        llvm::Function* fn = module->getRuntimeFunction("__cajeta_prof_instr_enter");
        if (!fn) return frame;

        auto& ctx = *module->getLlvmContext();
        llvm::Constant* tC = builder->CreateGlobalString(typeName);
        llvm::Constant* mC = builder->CreateGlobalString(methodName);
        llvm::Constant* fC = builder->CreateGlobalString(fileName);
        llvm::StructType* descTy = profMethodTy(ctx);
        llvm::Module* mod = builder->GetInsertBlock()->getModule();
        ensureInstrRegistered(mod, module);

        llvm::Type* ptrTy = llvm::PointerType::get(ctx, 0);
        llvm::Type* i64 = llvm::Type::getInt64Ty(ctx);
        llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
        llvm::Constant* init = llvm::ConstantStruct::get(
            descTy, {tC, mC, fC,
                     llvm::ConstantInt::get(i64, 0),
                     llvm::ConstantInt::get(i64, 0),
                     llvm::ConstantInt::get(i64, 0),
                     llvm::ConstantInt::get(i32, 0),
                     llvm::ConstantInt::get(i32, 0),
                     llvm::ConstantPointerNull::get(
                         llvm::cast<llvm::PointerType>(ptrTy))});
        frame.desc = new llvm::GlobalVariable(
            *mod, descTy, /*isConstant=*/false, llvm::GlobalValue::PrivateLinkage,
            init, ".cajeta.profmethod");

        llvm::Value* t0 = builder->CreateCall(fn, {frame.desc});
        // In the ENTRY block, so mem2reg can promote it.
        llvm::BasicBlock& entry = builder->GetInsertBlock()->getParent()->getEntryBlock();
        llvm::IRBuilder<> entryB(&entry, entry.getFirstInsertionPt());
        frame.t0Slot = entryB.CreateAlloca(i64, nullptr, "__prof_t0");
        builder->CreateStore(t0, frame.t0Slot);
        return frame;
    }

    void emitProfileExit(cajeta::CajetaModulePtr module, const ProfileFrame& frame) {
        if (!frame) return;
        llvm::IRBuilder<>* builder = profGuard(module);
        if (!builder) return;
        llvm::Function* fn = module->getRuntimeFunction("__cajeta_prof_instr_exit");
        if (!fn) return;
        llvm::Value* t0 = builder->CreateLoad(
            llvm::Type::getInt64Ty(*module->getLlvmContext()), frame.t0Slot);
        builder->CreateCall(fn, {frame.desc, t0});
    }

    void emitProfileExitAtReturns(cajeta::CajetaModulePtr module,
                                  llvm::Function* fn, const ProfileFrame& frame) {
        if (!frame || !fn) return;
        if (module->getFlags().profiler != Profiler::Instrument) return;
        llvm::Function* exitFn = module->getRuntimeFunction("__cajeta_prof_instr_exit");
        if (!exitFn) return;
        llvm::Type* i64 = llvm::Type::getInt64Ty(*module->getLlvmContext());
        for (llvm::BasicBlock& bb : *fn) {
            auto* ri = llvm::dyn_cast_or_null<llvm::ReturnInst>(bb.getTerminator());
            if (!ri) continue;
            llvm::IRBuilder<> b(ri);
            llvm::Value* t0 = b.CreateLoad(i64, frame.t0Slot);
            b.CreateCall(exitFn, {frame.desc, t0});
        }
    }

} // namespace cajeta::prof
