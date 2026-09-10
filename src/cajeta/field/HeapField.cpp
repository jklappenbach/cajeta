// HeapField - a Field whose value lives in a heap allocation.

#include "HeapField.h"
#include "../asn/VariableDeclarator.h"
#include "../compile/CajetaModule.h"
#include "../type/Scope.h"

namespace cajeta {

    llvm::AllocaInst* HeapField::getOrCreateAllocation() {
        // A field with no TYPE has no slot: session scope deliberately seeds a
        // binding NAME-ONLY when its canonical does not resolve in this unit.
        if (!type) return nullptr;
        if (!alloca) {
            // Entry-block alloca: a `heap` local in a loop must not re-allocate.
            alloca = module->createEntryAlloca(
                llvm::PointerType::get(type->getLlvmType()->getContext(), 0));
            if (initializer) {
                // A null from generateCode is a legitimate "no usable r-value",
                // so store an explicit null pointer: a null Value* would crash
                // inside CreateStore.
                llvm::Value* initVal = initializer->generateCode(module);
                if (!initVal) {
                    initVal = llvm::ConstantPointerNull::get(
                        llvm::PointerType::get(type->getLlvmType()->getContext(), 0));
                }
                module->getBuilder()->CreateStore(initVal, alloca);
            }
        }
        return alloca;
    }

    llvm::Value* HeapField::createStore(llvm::Value* value) {
        return nullptr;
    }

    llvm::Value* HeapField::createLoad() {
        if (alloca == nullptr) {
            alloca = this->getOrCreateAllocation();
        }
        return module->getBuilder()->CreateLoad(
            llvm::PointerType::get(type->getLlvmType()->getContext(), 0), alloca);
    }

    void HeapField::onDelete() {
        // A free emitted after a `ret` would leave dangling instructions and fail
        // LLVM verification, so a terminated block is skipped.
        auto* block = module->getBuilder()->GetInsertBlock();
        if (!block || block->hasTerminator()) {
            return;
        }
        MemoryManager::createFreeInstruction(module, createLoad(), block);
    }
}
