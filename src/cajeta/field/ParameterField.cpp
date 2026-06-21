//
// Created by James Klappenbach on 2/20/22.
//

#include "ParameterField.h"
#include "../compile/CajetaModule.h"
#include "../type/CajetaArray.h"
#include "../type/CajetaView.h"

namespace cajeta {

    ParameterField::ParameterField(CajetaModulePtr module, FormalParameterPtr formalParameter, llvm::Function* llvmFunction, int paramIndex) :
            Field(module, formalParameter->getName(), formalParameter->getType()),
            formalParameter(formalParameter) {
        reference = false;
        this->llvmFunction = llvmFunction;
        this->paramIndex = paramIndex;
    }


    llvm::Value* ParameterField::createLoad() {
        if (alloca == nullptr) {
            alloca = this->getOrCreateAllocation();
        }
        // @ValueType params travel by value: the slot holds the aggregate
        // INLINE (sized to the type), so a whole-value load reads the type,
        // not a pointer. Reference types load the `ptr` to the heap body.
        if (type->isValueType()) {
            return module->getBuilder()->CreateLoad(type->getLlvmType(), alloca);
        }
        return module->getBuilder()->CreateLoad(
            llvm::PointerType::get(*module->getLlvmContext(), 0), alloca);
    }

    llvm::Value* ParameterField::createStore(llvm::Value* value) {
        if (!alloca) {
            getOrCreateAllocation();
        }
        if (type->getTypeFlags() == PRIMITIVE_FLAG) {
            return module->getBuilder()->CreateStore(value, alloca);
        } else {
            return module->getBuilder()->CreateStore(value, alloca);
        }
    }

    llvm::AllocaInst* ParameterField::getOrCreateAllocation() {
        if (!alloca) {
            // Match the ABI Method::generatePrototype chose for this slot:
            // class/array (non-struct) parameters are passed as `ptr`, so
            // the local slot is `ptr` too. Otherwise (primitives, structs)
            // alloc the type itself.
            bool isStruct = dynamic_pointer_cast<CajetaView>(type) != nullptr;
            bool isArr = dynamic_pointer_cast<CajetaArray>(type) != nullptr;
            bool isClassLike = dynamic_pointer_cast<CajetaClass>(type) != nullptr;
            bool isPrim = type && (type->getTypeFlags() & PRIMITIVE_FLAG);
            // @ValueType params are passed BY VALUE (the aggregate), matching the
            // signature Method::generatePrototype emits — so the slot must be the
            // aggregate type, NOT `ptr`. A ptr-sized slot taking a by-value store
            // overflows into the next param slot and corrupts both operands.
            bool passByPointer = (isClassLike && !isStruct) && (isArr || !isPrim)
                && !type->isValueType();
            llvm::Type* llvmType;
            if (passByPointer) {
                llvmType = llvm::PointerType::get(*module->getLlvmContext(), 0);
            } else if ((type->getTypeFlags() & PRIMITIVE_FLAG)
                    || type->isValueType()) {
                llvmType = type->getLlvmType();
            } else {
                llvmType = llvm::PointerType::get(*module->getLlvmContext(), 0);
            }
            // Entry-block slot alloca: getOrCreateAllocation is lazy (first
            // reference), which can be inside a loop body — an alloca there
            // would re-allocate stack each iteration (overflow at -O0). The
            // store stays at the current point; it just references the
            // entry-block slot (which dominates every use).
            alloca = module->createEntryAlloca(llvmType);
            module->getBuilder()->CreateStore(llvmFunction->getArg(paramIndex), alloca);
        }
        return alloca;
    }
}
