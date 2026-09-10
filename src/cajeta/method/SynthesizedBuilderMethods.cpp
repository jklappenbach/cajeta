#include "SynthesizedBuilderMethods.h"
#include "../type/CajetaClass.h"
#include "../type/CajetaView.h"
#include "../type/CajetaArray.h"
#include "../type/FormalParameter.h"
#include "../compile/CajetaModule.h"
#include "../error/Exception.h"

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/DataLayout.h>

using namespace std;

namespace cajeta {

    // ---- Chained setter on the Builder ---------------------------------

    SynthesizedBuilderSetterMethod::SynthesizedBuilderSetterMethod(
            CajetaModulePtr module, CajetaClassPtr builder,
            StructurePropertyPtr field,
            const std::string& methodName)
        : Method(module, methodName,
                 std::static_pointer_cast<CajetaType>(builder),
                 builder),
          field(field) {
        this->parent = builder;
    }

    void SynthesizedBuilderSetterMethod::initParameter() {
        if (!parameterList.empty()) return;
        auto valueParam = make_shared<FormalParameter>(
            field->getName(), field->getType());
        valueParam->setParent(shared_from_this());
        parameterList.push_back(valueParam);
        parameters[valueParam->getName()] = valueParam;
    }

    void SynthesizedBuilderSetterMethod::generateCode() {
        auto& llvmFunction = llvmFunctionRef();  // U6.3b: frozen-aware
        // (this, value) -> ptr: stores value into the named field, returns this.
        llvm::LLVMContext& ctx = *module->getLlvmContext();
        llvmBasicBlock = llvm::BasicBlock::Create(ctx, "entry", llvmFunction);
        llvm::IRBuilder<> b(llvmBasicBlock);

        llvm::Value* thisPtr = llvmFunction->getArg(0);
        llvm::Value* value   = llvmFunction->getArg(1);

        int idx = parent->getFieldLlvmIndex(field);
        if (idx < 0) {
            throw Exception(
                "@Builder synthesizer: setter field '" + field->getName()
                + "' has no LLVM index on '"
                + parent->getQName()->toCanonical() + "'",
                "CAJETA_ERROR_BUILDER_SETTER_FIELD_INDEX");
        }
        llvm::Value* slot = b.CreateStructGEP(
            parent->getLlvmType(), thisPtr, (unsigned) idx,
            std::string("bset.") + field->getName());
        b.CreateStore(value, slot);
        b.CreateRet(thisPtr);
    }

    // ---- build() — construct the outer instance ------------------------

    SynthesizedBuildMethod::SynthesizedBuildMethod(
            CajetaModulePtr module, CajetaClassPtr builder,
            CajetaClassPtr outer,
            const std::string& methodName)
        : Method(module, methodName,
                 std::static_pointer_cast<CajetaType>(outer),
                 builder),
          outer(outer) {
        this->parent = builder;
        // build() hands back a fresh allocation the caller must drop.
        this->setReturnsOwnership(true);
    }

    void SynthesizedBuildMethod::generateCode() {
        auto& llvmFunction = llvmFunctionRef();  // U6.3b: frozen-aware
        // Emits `heap Outer(...)` by hand: alloc, vtable into slot 0, the outer's
        // all-args ctor over the builder's fields, then return the instance.
        llvm::LLVMContext& ctx = *module->getLlvmContext();
        llvmBasicBlock = llvm::BasicBlock::Create(ctx, "entry", llvmFunction);
        llvm::IRBuilder<> b(llvmBasicBlock);
        llvm::Module* lmod = module->getLlvmModule();
        const llvm::DataLayout& dl = lmod->getDataLayout();

        llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
        llvm::PointerType* ptrTy = llvm::PointerType::get(ctx, 0);

        llvm::Function* allocFn = module->getRuntimeFunction("__cajeta_alloc");
        if (!allocFn) {
            throw Exception(
                "@Builder synthesizer: runtime helper __cajeta_alloc not linked",
                "CAJETA_ERROR_BUILDER_RUNTIME");
        }

        llvm::Type* outerLlvm = outer->getLlvmType();
        uint64_t outerSize = dl.getTypeAllocSize(outerLlvm);

        llvm::Value* newOuter = b.CreateCall(allocFn,
            { llvm::ConstantInt::get(i64Ty, outerSize) }, "build.alloc");

        llvm::GlobalVariable* vt = outer->getVirtualTableGlobal();
        if (vt) {
            llvm::Value* vtSlot = b.CreateStructGEP(outerLlvm, newOuter, 0,
                "build.vt.slot");
            // The vtable global must be reachable from THIS module.
            llvm::Constant* vtRef = CajetaModule::ensureGlobalInModule(
                lmod, vt);
            b.CreateStore(vtRef, vtSlot);
        }

        // The all-args ctor: one parameter per non-static field, after `this`.
        std::vector<StructurePropertyPtr> outerFields;
        for (auto& prop : outer->getPropertyList()) {
            if (!prop || prop->isStatic()) continue;
            outerFields.push_back(prop);
        }
        size_t expectedArity = 1 + outerFields.size();  // this + N fields

        MethodPtr ctor;
        for (auto& bucket : outer->getMethods()) {
            MethodPtr m = bucket.second;
            if (!m || !m->isConstructor()) continue;
            auto params = m->getParameterList();
            if (params.size() != expectedArity) continue;
            ctor = m;
            break;
        }
        if (!ctor) {
            throw Exception(
                "@Builder synthesizer: no all-args constructor found on `"
                + outer->getQName()->toCanonical()
                + "` (expected " + std::to_string(outerFields.size())
                + " user params); @Builder should have synthesized one — "
                "this is a compiler bug",
                "CAJETA_ERROR_BUILDER_NO_CTOR");
        }
        llvm::Function* ctorFn = ctor->getLlvmFunction();
        if (!ctorFn) {
            throw Exception(
                "@Builder synthesizer: outer's all-args ctor has no "
                "LLVM function",
                "CAJETA_ERROR_BUILDER_CTOR_FN");
        }
        ctorFn = CajetaModule::ensureFunctionInModule(lmod, ctorFn);

        llvm::Type* builderLlvm = parent->getLlvmType();
        llvm::Value* thisPtr = llvmFunction->getArg(0);

        std::vector<llvm::Value*> ctorArgs;
        ctorArgs.push_back(newOuter);
        for (auto& prop : outerFields) {
            // The Builder mirrors outer's fields by NAME, not by slot order.
            StructurePropertyPtr builderProp;
            for (auto& bp : parent->getPropertyList()) {
                if (bp && bp->getName() == prop->getName()) {
                    builderProp = bp;
                    break;
                }
            }
            if (!builderProp) {
                throw Exception(
                    "@Builder synthesizer: Builder lacks mirror field `"
                    + prop->getName() + "`",
                    "CAJETA_ERROR_BUILDER_MIRROR_MISSING");
            }
            int bidx = parent->getFieldLlvmIndex(builderProp);
            llvm::Value* slot = b.CreateStructGEP(
                builderLlvm, thisPtr, (unsigned) bidx,
                std::string("build.read.") + prop->getName());

            // Storage shape: arrays and class refs load as ptr, the rest native.
            CajetaTypePtr ft = prop->getType();
            llvm::Type* loadTy;
            bool slotIsPtr = false;
            if (dynamic_pointer_cast<CajetaArray>(ft)) {
                loadTy = ptrTy; slotIsPtr = true;
            } else if (auto cls = dynamic_pointer_cast<CajetaClass>(ft)) {
                if (dynamic_pointer_cast<CajetaView>(ft)) {
                    loadTy = ft->getLlvmType();
                } else if (cls->isInterface()) {
                    loadTy = ft->getLlvmType();
                } else {
                    loadTy = ptrTy; slotIsPtr = true;
                }
            } else {
                loadTy = ft->getLlvmType();
            }
            (void) slotIsPtr;
            llvm::Value* v = b.CreateLoad(loadTy, slot,
                std::string("build.v.") + prop->getName());
            ctorArgs.push_back(v);
        }
        // All-ones transfer word: build() surrenders its collected values to the
        // instance. The word cannot be omitted — the .bc writer does not verify, so
        // an arg-count mismatch surfaces only as a reader error on cache reload.
        if (ctor->needsTransferWord()) {
            uint64_t allOwned = ctorArgs.size() > 1
                ? ((1ull << (ctorArgs.size() - 1)) - 1) : 0;
            ctorArgs.push_back(llvm::ConstantInt::get(
                llvm::Type::getInt64Ty(ctx), allOwned));
        }

        b.CreateCall(ctorFn, ctorArgs);
        b.CreateRet(newOuter);
    }

    // ---- builder() — static factory on outer ----------------------------

    SynthesizedBuilderFactoryMethod::SynthesizedBuilderFactoryMethod(
            CajetaModulePtr module, CajetaClassPtr outer,
            CajetaClassPtr builder,
            const std::string& methodName,
            std::vector<DefaultEntry> defaults)
        : Method(module, methodName,
                 std::static_pointer_cast<CajetaType>(builder),
                 outer),
          builder(builder),
          defaults(std::move(defaults)) {
        this->parent = outer;
        // Static, so generatePrototype inserts no implicit `this`.
        this->addModifier(STATIC);
    }

    void SynthesizedBuilderFactoryMethod::generateCode() {
        auto& llvmFunction = llvmFunctionRef();  // U6.3b: frozen-aware
        // () -> ptr: alloc, vtable, then the @Builder.Default initializers, which
        // run here so an untouched setter still shows its declared default. The
        // no-arg ctor is skipped: the allocation already zero-fills.
        llvm::LLVMContext& ctx = *module->getLlvmContext();
        llvmBasicBlock = llvm::BasicBlock::Create(ctx, "entry", llvmFunction);
        llvm::IRBuilder<> b(llvmBasicBlock);
        llvm::Module* lmod = module->getLlvmModule();
        const llvm::DataLayout& dl = lmod->getDataLayout();

        llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);

        llvm::Function* allocFn = module->getRuntimeFunction("__cajeta_alloc");
        if (!allocFn) {
            throw Exception(
                "@Builder synthesizer: runtime helper __cajeta_alloc not linked",
                "CAJETA_ERROR_BUILDER_RUNTIME");
        }
        llvm::Type* builderLlvm = builder->getLlvmType();
        uint64_t builderSize = dl.getTypeAllocSize(builderLlvm);

        llvm::Value* newBuilder = b.CreateCall(allocFn,
            { llvm::ConstantInt::get(i64Ty, builderSize) }, "bfact.alloc");

        llvm::GlobalVariable* vt = builder->getVirtualTableGlobal();
        if (vt) {
            llvm::Value* vtSlot = b.CreateStructGEP(builderLlvm, newBuilder, 0,
                "bfact.vt.slot");
            llvm::Constant* vtRef = CajetaModule::ensureGlobalInModule(
                lmod, vt);
            b.CreateStore(vtRef, vtSlot);
        }

        // An initializer's generateCode emits through the MODULE's builder, so the
        // local one is swapped in for the duration and restored after.
        if (!defaults.empty()) {
            auto* prevBuilder = module->getBuilder();
            module->setBuilder(&b);
            for (auto& entry : defaults) {
                if (!entry.mirrorField || !entry.initializer) continue;
                int idx = builder->getFieldLlvmIndex(entry.mirrorField);
                if (idx < 0) continue;
                llvm::Value* initVal = entry.initializer->generateCode(module);
                if (!initVal) continue;
                llvm::Value* slot = b.CreateStructGEP(
                    builderLlvm, newBuilder, (unsigned) idx,
                    std::string("bfact.def.") + entry.mirrorField->getName());
                // A literal's natural width (i64, f64) need not be the slot's.
                llvm::Type* slotTy = entry.mirrorField->getType()
                    ? entry.mirrorField->getType()->getLlvmType()
                    : nullptr;
                if (slotTy && initVal->getType() != slotTy) {
                    llvm::Type* srcTy = initVal->getType();
                    if (slotTy->isIntegerTy() && srcTy->isIntegerTy()) {
                        initVal = b.CreateIntCast(initVal, slotTy, /*isSigned=*/true);
                    } else if (slotTy->isFloatingPointTy() && srcTy->isFloatingPointTy()) {
                        initVal = b.CreateFPCast(initVal, slotTy);
                    } else if (slotTy->isFloatingPointTy() && srcTy->isIntegerTy()) {
                        initVal = b.CreateSIToFP(initVal, slotTy);
                    } else if (slotTy->isIntegerTy() && srcTy->isFloatingPointTy()) {
                        initVal = b.CreateFPToSI(initVal, slotTy);
                    }
                }
                b.CreateStore(initVal, slot);
            }
            module->setBuilder(prevBuilder);
        }

        b.CreateRet(newBuilder);
    }
}
