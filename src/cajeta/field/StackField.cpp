//
// Created by James Klappenbach on 3/22/23.
//

#include "StackField.h"
#include "../type/CajetaType.h"
#include "../compile/CajetaModule.h"
#include "../asn/VariableDeclarator.h"

namespace cajeta {
    StackField::StackField(CajetaModulePtr module,
        string name,
        CajetaTypePtr type,
        bool reference,
        set<Modifier> modifiers,
        set<QualifiedNamePtr> annotations,
        InitializerPtr initializer) : Field(module,
            name,
            type,
            reference,
            modifiers,
            annotations,
            initializer) {

    }

    /**
     * I think this is how variable allocations should be handled, either way: the initializer will
     * either create a stack origin, or one off the heap created by malloc
     *
     * @return
     */
    llvm::AllocaInst* StackField::getOrCreateAllocation() {
        if (!alloca) {
            if (type->getTypeFlags() & PRIMITIVE_FLAG) {
                alloca = module->getBuilder()->CreateAlloca(type->getLlvmType());
            } else {
                alloca = module->getBuilder()->CreateAlloca(type->getLlvmType()->getPointerTo());
            }
            if (initializer != nullptr) {
                module->getBuilder()->CreateStore(initializer->generateCode(module), alloca);
            }
        }
        return alloca;
    }

    llvm::Value* StackField::createLoad() {
        if (!alloca) {
            getOrCreateAllocation();
        }
        if (type->getTypeFlags() & PRIMITIVE_FLAG) {
            return module->getBuilder()->CreateLoad(type->getLlvmType(), alloca);
        } else {
            return module->getBuilder()->CreateLoad(type->getLlvmType()->getPointerTo(), alloca);
        }
    }

    llvm::Value* StackField::createStore(llvm::Value* value) {
        if (!alloca) {
            getOrCreateAllocation();
        }
        if (type->getTypeFlags() & PRIMITIVE_FLAG) {
            return module->getBuilder()->CreateStore(value, alloca);
        } else {
            return module->getBuilder()->CreateStore(value, alloca);
        }
    }
} // code