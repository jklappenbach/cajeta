//
// BoundClosureField — slot-backed forwarded reads. See BoundClosureField.h.
//

#include "BoundClosureField.h"

#include "cajeta/compile/CajetaModule.h"

namespace cajeta {

    llvm::AllocaInst* BoundClosureField::getOrCreateAllocation() {
        if (slot || !closureRecord) return slot;
        auto* ptrTy = llvm::PointerType::get(*module->getLlvmContext(), 0);
        slot = module->createEntryAlloca(ptrTy, name + ".bound");
        // Store the constant record right after the alloca (entry block) so it
        // dominates every forwarded use in the body.
        llvm::IRBuilder<> entryBuilder(slot->getParent(), std::next(slot->getIterator()));
        entryBuilder.CreateStore(closureRecord, slot);
        return slot;
    }

    llvm::Value* BoundClosureField::createLoad() {
        return closureRecord ? static_cast<llvm::Value*>(closureRecord)
                             : static_cast<llvm::Value*>(boundFunction);
    }

}
