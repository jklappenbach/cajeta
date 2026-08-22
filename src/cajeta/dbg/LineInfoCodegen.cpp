#include "cajeta/dbg/LineInfoCodegen.h"

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

        // cajeta-profiler 4.2.e (spec §2.5). The profiler must refuse to arm on
        // a --line-info=off binary rather than produce an empty trace, and the
        // runtime cannot infer the flag: an empty shadow stack is what an idle
        // program looks like too, and counting probe calls would put a cost on
        // the hot path §2.9 forbids. So codegen says so, once per module.
        //
        // A ctor, NOT a weak extern. cajeta_rt_core.c's loc-table note has the
        // reason: the runtime bitcode is linked into each module long before
        // codegen emits its definition, so a weak default there and a strong one
        // here collide in one module and LLVM silently renames the second. A
        // ctor behaves identically under LLJIT and a native link — the shape the
        // loc table and the XPU kernel registry both already use.
        //
        // Idempotent by construction: emitLineEnter runs per method, and the
        // presence of the ctor function is the "already done" flag.
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
        // #FrameDesc { i8* typeName, i8* methodName, i8* fileName } — one private
        // constant per method, program lifetime; matches CajetaFrameDesc in
        // cajeta_rt_core.c. The strings land in the builder's current module.
        llvm::Constant* tC = builder->CreateGlobalString(typeName);
        llvm::Constant* mC = builder->CreateGlobalString(methodName);
        llvm::Constant* fC = builder->CreateGlobalString(fileName);
        llvm::StructType* descTy = llvm::StructType::get(ctx, {ptrTy, ptrTy, ptrTy});
        llvm::Module* mod = builder->GetInsertBlock()->getModule();
        // First probe in this module also records that probes exist at all.
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
        // The per-STATEMENT mark is the entire runtime cost of the shadow
        // stack, and it is not close. Measured 2026-08-22 at -O3:
        //
        //                       off     enter/leave    + marks
        //   realistic body     0.11 s      0.11 s       0.39 s   (3.5x)
        //   tiny callee        0.05 s      0.15 s       0.47 s   (9.4x)
        //
        // Per-CALL enter/leave is at parity with an uninstrumented build on
        // ordinary code; per-statement marks cost 3.5-9.4x. And the cost is
        // NOT the probe's work — with the bodies emptied the figure is
        // unchanged, because an opaque call at every statement boundary
        // forbids inlining and folding. There is no cheap version to engineer
        // toward, only a decision about when to emit them at all.
        //
        // So they are now a --debug-info=full feature. `line` (the default,
        // and what the release flavor selects) keeps the frame identity that
        // makes a trace name Type.method(File.cajeta) and that the profiler
        // samples; `full` adds the exact line. This is what the flag's own
        // note in CompilerMode.h asked for: "measure before relying on
        // default-on in release."
        if (!module->getFlags().debugInfo) return;
        llvm::IRBuilder<>* builder = lineGuard(module);
        if (!builder || line <= 0) return;
        llvm::Function* fn = module->getRuntimeFunction("__cajeta_line_mark");
        if (!fn) return;
        builder->CreateCall(fn, {builder->getInt32(line)});
    }

} // namespace cajeta::dbg
