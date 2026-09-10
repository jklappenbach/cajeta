//
// Created by James Klappenbach on 2/19/22.
//

#include "../error/Diagnostics.h"
#include "DefaultConstructorMethod.h"
#include "../type/CajetaClass.h"
#include "../asn/DefaultBlock.h"
#include "../compile/CajetaModule.h"

using namespace std;

namespace cajeta {
    /** Names the synthesized default after the template ORIGIN's simple name, so
     *  a ctor-less instantiation resolves `Stream(...)` rather than the
     *  arg-suffixed typeName the instantiation itself carries. */
    DefaultConstructorMethod::DefaultConstructorMethod(CajetaModulePtr module, CajetaClassPtr parent)
        : Method(module,
              parent->getTemplateOrigin()
                  ? parent->getTemplateOrigin()->getQName()->getTypeName()
                  : parent->getQName()->getTypeName(),
              CajetaType::of("void"), parent) {
        this->parent = parent;
        constructor = true;
        block = make_shared<DefaultBlock>();
    }

    /** Emits the default constructor body: scope, default block, then the field
     *  initializers a class with no declared constructor would otherwise lose. */
    void DefaultConstructorMethod::generateCode() {
        auto& llvmFunction = llvmFunctionRef();
        llvmBasicBlock = llvm::BasicBlock::Create(*module->getLlvmContext(), name, llvmFunction);
        builder = new llvm::IRBuilder<>(llvmBasicBlock, llvmBasicBlock->begin());
        builder->SetInsertPoint(llvmBasicBlock);
        module->setBuilder(builder);
        module->setCurrentMethod(shared_from_this());

        createScope();
        block->generateCode(module);

        if (parent && llvmFunction->arg_size() > 0) {
            llvm::Value* thisPtr = llvmFunction->getArg(0);
            for (auto& prop : parent->getPropertyList()) {
                if (!prop || prop->isStatic()) continue;
                auto init = prop->getInitializer();
                if (!init) continue;
                int idx = parent->getFieldLlvmIndex(prop);
                if (idx < 0) continue;
                llvm::Value* initVal = init->generateCode(module);
                if (!initVal) {
                    throw locatedException(
                        init->getSourceLine(), init->getSourceColumn() + 1,
                        "initializer for field '" + prop->getName()
                            + "' did not resolve to a value",
                        "CAJETA_ERROR_UNRESOLVED_EXPRESSION");
                }
                auto* b = module->getBuilder();
                llvm::Value* fp = b->CreateStructGEP(
                    parent->getLlvmType(), thisPtr, (unsigned) idx,
                    std::string("default_ctor.init.") + prop->getName());
                CajetaTypePtr ft = prop->getType();
                llvm::Type* slotTy = ft ? ft->getLlvmType() : nullptr;
                if (slotTy && initVal->getType() != slotTy) {
                    llvm::Type* srcTy = initVal->getType();
                    if (slotTy->isIntegerTy() && srcTy->isIntegerTy()) {
                        initVal = b->CreateIntCast(initVal, slotTy, /*isSigned=*/true);
                    } else if (slotTy->isFloatingPointTy() && srcTy->isFloatingPointTy()) {
                        initVal = b->CreateFPCast(initVal, slotTy);
                    } else if (slotTy->isFloatingPointTy() && srcTy->isIntegerTy()) {
                        initVal = b->CreateSIToFP(initVal, slotTy);
                    } else if (slotTy->isIntegerTy() && srcTy->isFloatingPointTy()) {
                        initVal = b->CreateFPToSI(initVal, slotTy);
                    }
                }
                b->CreateStore(initVal, fp);
            }
        }

        destroyScope();

        module->getBuilder()->CreateRetVoid();
    }
}