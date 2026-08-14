//
// Created by James Klappenbach on 2/20/22.
//

#include "HeapField.h"
#include "../asn/VariableDeclarator.h"
#include "../compile/CajetaModule.h"
#include "../type/Scope.h"

namespace cajeta {

    llvm::AllocaInst* HeapField::getOrCreateAllocation() {
        // A field with no TYPE has no slot to make. That is not a defensive
        // nicety: `seedSessionScope` deliberately seeds a session binding
        // NAME-ONLY when its recorded canonical does not resolve in this
        // unit's type world (script-units §4 — the ownership checks need the
        // name, and a live read rejects before the type is ever touched).
        // Every seeded field lands here through `Scope::putField`, so without
        // this the documented name-only path dereferenced a null type and
        // took the process down. Found 2026-08-14 by
        // SessionOwnershipTests.moveStateSpansUnits.
        if (!type) return nullptr;
        if (!alloca) {
            // Entry-block slot alloca (see CajetaModule::createEntryAlloca):
            // a `heap` local in a loop body must not re-allocate stack per pass.
            alloca = module->createEntryAlloca(
                llvm::PointerType::get(type->getLlvmType()->getContext(), 0));
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
        // Skip if the current block is already terminated — emitting a free after a
        // `ret` would leave dangling instructions and break LLVM verification. This is
        // a stopgap until ownership analysis decides per-field whether a free is
        // appropriate; for now no free fires on functions that return their array.
        auto* block = module->getBuilder()->GetInsertBlock();
        if (!block || block->hasTerminator()) {
            return;
        }
        MemoryManager::createFreeInstruction(module, createLoad(), block);
    }
}
