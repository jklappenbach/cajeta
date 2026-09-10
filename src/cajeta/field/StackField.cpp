// StackField - a Field whose value lives in a stack allocation.

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
     * Create the slot once — inline for value types, `ptr` otherwise — and initialize it.
     */
    llvm::AllocaInst* StackField::getOrCreateAllocation() {
        if (!type) return nullptr;
        if (!alloca) {
            // The slot alloca goes in the function ENTRY block, not at the
            // current point: a local declared inside a loop body would
            // otherwise re-allocate native stack every iteration.
            if (type->hasValueSemantics()) {
                alloca = module->createEntryAlloca(type->getLlvmType());
            } else {
                alloca = module->createEntryAlloca(
                    llvm::PointerType::get(*module->getLlvmContext(), 0));
            }
            if (initializer != nullptr) {
                llvm::Value* initVal = initializer->generateCode(module);
                if (initVal) {
                    // `stack Vec2(...)` yields a POINTER to a fresh aggregate but
                    // a value-type slot holds it BY VALUE: load before storing.
                    llvm::Type* fieldTy = alloca->getAllocatedType();
                    if (type->isValueType() && initVal->getType()->isPointerTy()
                            && fieldTy->isAggregateType()) {
                        initVal = module->getBuilder()->CreateLoad(fieldTy, initVal);
                    }
                    if (initVal->getType() != fieldTy) {
                        auto* builder = module->getBuilder();
                        llvm::Type* srcTy = initVal->getType();
                        if (fieldTy->isIntegerTy() && srcTy->isIntegerTy()) {
                            initVal = builder->CreateIntCast(initVal, fieldTy, /*isSigned=*/true);
                        } else if (fieldTy->isFloatingPointTy() && srcTy->isFloatingPointTy()) {
                            initVal = builder->CreateFPCast(initVal, fieldTy);
                        } else if (fieldTy->isFloatingPointTy() && srcTy->isIntegerTy()) {
                            initVal = builder->CreateSIToFP(initVal, fieldTy);
                        } else if (fieldTy->isIntegerTy() && srcTy->isFloatingPointTy()) {
                            initVal = builder->CreateFPToSI(initVal, fieldTy);
                        }
                    }
                    module->getBuilder()->CreateStore(initVal, alloca);
                }
            }
        }
        return alloca;
    }

    llvm::Value* StackField::createLoad() {
        if (!alloca) {
            getOrCreateAllocation();
        }
        if (type->hasValueSemantics()) {
            return module->getBuilder()->CreateLoad(type->getLlvmType(), alloca);
        } else {
            return module->getBuilder()->CreateLoad(
                llvm::PointerType::get(*module->getLlvmContext(), 0), alloca);
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