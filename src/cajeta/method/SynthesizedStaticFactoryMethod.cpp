#include "SynthesizedStaticFactoryMethod.h"
#include "SynthesizedConstructorMethod.h"
#include "../type/CajetaClass.h"
#include "../type/FormalParameter.h"
#include "../compile/CajetaModule.h"
#include "../util/MemoryManager.h"
#include "../error/Exception.h"

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/DataLayout.h>

using namespace std;

namespace cajeta {

    SynthesizedStaticFactoryMethod::SynthesizedStaticFactoryMethod(
            CajetaModulePtr module, CajetaClassPtr parent,
            std::shared_ptr<SynthesizedConstructorMethod> ctor,
            const std::string& methodName,
            std::vector<StructurePropertyPtr> fields)
        : Method(module, methodName,
                 std::static_pointer_cast<CajetaType>(parent),
                 parent),
          ctor(ctor),
          fields(std::move(fields)) {
        this->parent = parent;
        // Static: no implicit `this` insertion in Method::generatePrototype.
        this->addModifier(STATIC);
        // The factory returns a freshly-malloc'd heap instance; signal
        // ownership transfer so the caller's drop chain handles it.
        this->setReturnsOwnership(true);
    }

    void SynthesizedStaticFactoryMethod::initParameters() {
        if (!parameterList.empty()) return;
        for (auto& f : fields) {
            auto p = make_shared<FormalParameter>(f->getName(), f->getType());
            p->setParent(shared_from_this());
            parameterList.push_back(p);
            parameters[p->getName()] = p;
        }
    }

    void SynthesizedStaticFactoryMethod::generateCode() {
        // Static: (arg1, ..., argN) -> ptr (parent). Alloc the parent
        // class on the heap, init its vtable, call the wrapped ctor
        // with `(instance, arg1, ..., argN)`, return the instance.
        llvm::LLVMContext& ctx = *module->getLlvmContext();
        llvmBasicBlock = llvm::BasicBlock::Create(ctx, "entry", llvmFunction);
        llvm::IRBuilder<> b(llvmBasicBlock);
        llvm::Module* lmod = module->getLlvmModule();
        const llvm::DataLayout& dl = lmod->getDataLayout();

        llvm::Type* parentTy = parent->getLlvmType();
        if (!parentTy) {
            throw Exception(
                "@<Ctor>(staticName) factory: parent class `"
                + parent->getQName()->toCanonical()
                + "` has no LLVM struct type at codegen time",
                "CAJETA_ERROR_STATIC_FACTORY_NO_LAYOUT");
        }

        // Alloc + vtable init — mirrors ComponentInjectMethod's
        // fresh-allocation path and ClassCreatorRest's `heap T(...)`
        // codegen. The malloc lands in the per-thread live-set so the
        // drop chain reclaims it when ownership terminates.
        llvm::Constant* allocSize = llvm::ConstantInt::get(
            llvm::Type::getInt64Ty(ctx), dl.getTypeAllocSize(parentTy));
        llvm::CallInst* instance = MemoryManager::createMallocInstruction(
            module, allocSize, llvmBasicBlock);

        if (auto vt = parent->getVirtualTableGlobal()) {
            llvm::Constant* vtRef = CajetaModule::ensureGlobalInModule(
                lmod, vt);
            llvm::Value* vtSlot = b.CreateStructGEP(
                parentTy, instance, /*idx=*/0, "sf.vt.slot");
            b.CreateStore(vtRef, vtSlot);
        }

        // Direct call to the wrapped ctor's LLVM function. The ctor's
        // signature is `(ptr this, arg1, ..., argN)`. Cross-module
        // safe via ensureFunctionInModule (matches the @Encoding
        // synth's encode/decode call shape).
        llvm::Function* ctorFn = ctor ? ctor->getLlvmFunction() : nullptr;
        if (!ctorFn) {
            throw Exception(
                "@<Ctor>(staticName) factory: wrapped ctor on `"
                + parent->getQName()->toCanonical()
                + "` has no LLVM function (codegen ordering bug)",
                "CAJETA_ERROR_STATIC_FACTORY_NO_CTOR");
        }
        ctorFn = CajetaModule::ensureFunctionInModule(lmod, ctorFn);

        std::vector<llvm::Value*> callArgs;
        callArgs.push_back(instance);
        for (unsigned i = 0; i < llvmFunction->arg_size(); ++i) {
            callArgs.push_back(llvmFunction->getArg(i));
        }
        b.CreateCall(ctorFn, callArgs);

        b.CreateRet(instance);
    }
}
