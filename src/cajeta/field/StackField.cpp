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
            // Storage axis (plans/value-type-overloading-plan.md): scalar
            // primitives AND @ValueType PODs live INLINE in the slot (the
            // value itself), loaded/stored whole. Reference types (classes,
            // arrays) keep a `ptr` slot holding the heap body pointer.
            // The slot alloca goes in the function ENTRY block (not the current
            // point): a local declared inside a loop body would otherwise
            // re-allocate native stack every iteration -> overflow at -O0.
            if (type->hasValueSemantics()) {
                alloca = module->createEntryAlloca(type->getLlvmType());
            } else {
                alloca = module->createEntryAlloca(
                    llvm::PointerType::get(*module->getLlvmContext(), 0));
            }
            if (initializer != nullptr) {
                llvm::Value* initVal = initializer->generateCode(module);
                if (initVal) {
                    // @ValueType construction (`stack Vec2(...)`) yields a
                    // POINTER to a freshly-built aggregate, but a value-type
                    // local's slot holds the aggregate BY VALUE — load the
                    // value through the pointer before storing (a copy into
                    // the inline slot). Value types are Copy. Primitives and
                    // operator-result aggregates already arrive by value.
                    llvm::Type* fieldTy = alloca->getAllocatedType();
                    if (type->isValueType() && initVal->getType()->isPointerTy()
                            && fieldTy->isAggregateType()) {
                        initVal = module->getBuilder()->CreateLoad(fieldTy, initVal);
                    }
                    // Coerce when the initializer's natural LLVM type doesn't match the
                    // declared field type — e.g. integer literals default to i64 but a
                    // field declared int32 needs the value truncated.
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
        // By-value types (primitives + @ValueType PODs) load the inline value;
        // reference types load the heap pointer held in the slot.
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