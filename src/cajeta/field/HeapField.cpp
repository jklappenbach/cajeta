//
// Created by James Klappenbach on 2/20/22.
//

#include "HeapField.h"
#include "../asn/VariableDeclarator.h"
#include "../compile/CajetaModule.h"
#include "../type/Scope.h"

namespace cajeta {

    llvm::AllocaInst* HeapField::getOrCreateAllocation() {
        if (!alloca) {
            alloca = module->getBuilder()->CreateAlloca(type->getLlvmType()->getPointerTo());
            if (initializer) {
                // An initializer whose generateCode returns null is a
                // legitimate "no usable r-value" — e.g. a `s.foo` on a
                // class whose layout has no `foo` field (path-borrow
                // tests intentionally exercise that shape). Fall back
                // to storing an explicit null pointer of the slot's
                // type rather than feeding a null Value* into
                // CreateStore, which would crash inside IRBuilder.
                llvm::Value* initVal = initializer->generateCode(module);
                if (!initVal) {
                    initVal = llvm::ConstantPointerNull::get(
                        llvm::cast<llvm::PointerType>(
                            type->getLlvmType()->getPointerTo()));
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
        return module->getBuilder()->CreateLoad(type->getLlvmType()->getPointerTo(), alloca);
    }

    void HeapField::onDelete() {
        // Skip if the current block is already terminated — emitting a free after a
        // `ret` would leave dangling instructions and break LLVM verification. This is
        // a stopgap until ownership analysis decides per-field whether a free is
        // appropriate; for now no free fires on functions that return their array.
        auto* block = module->getBuilder()->GetInsertBlock();
        if (!block || block->getTerminator()) {
            return;
        }
        MemoryManager::createFreeInstruction(module, createLoad(), block);
    }
}
