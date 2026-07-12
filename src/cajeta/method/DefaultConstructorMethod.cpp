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
    /**
     * Default constructor method
     * @param name
     * @param returnType
     * @param parent
     */
    DefaultConstructorMethod::DefaultConstructorMethod(CajetaModulePtr module, CajetaClassPtr parent)
        : Method(module, parent->getQName()->getTypeName(), CajetaType::of("void"), parent) {
        this->parent = parent;
        this->name = name;
        this->returnType = returnType;
        constructor = parent->getQName()->getTypeName() == name;
        block = make_shared<DefaultBlock>();
    }

    void DefaultConstructorMethod::generateCode() {
        auto& llvmFunction = llvmFunctionRef();  // U6.3b: frozen-aware
        llvmBasicBlock = llvm::BasicBlock::Create(*module->getLlvmContext(), name, llvmFunction);
        builder = new llvm::IRBuilder<>(llvmBasicBlock, llvmBasicBlock->begin());
        builder->SetInsertPoint(llvmBasicBlock);
        module->setBuilder(builder);
        module->setCurrentMethod(shared_from_this());

        createScope();
        block->generateCode(module);

        // Field initializers. A class with NO declared constructor gets this
        // default one — and it used to emit an empty body, so `int32 v = 3;`
        // was silently DROPPED and the field read back 0 (the allocator's
        // memset was the only thing that ever wrote it). Declaring an empty
        // `Holder() { }` changed the field's value, which no user would expect.
        // The user-ctor path (Method.cpp) and the synthesized-ctor path
        // (SynthesizedConstructorMethod.cpp) both run this loop; this one did
        // not. Found by silent-resolution diagnostics 3.1.3.
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