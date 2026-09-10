// StackField - a Field whose value lives in a stack allocation.

#pragma once
#include "Field.h"

namespace cajeta {

    class StackField : public Field, public enable_shared_from_this<Field> {
    public:
        StackField(CajetaModulePtr module, string name, CajetaTypePtr type, FieldPtr parent = nullptr) : Field(module, name, type, parent) { }

        StackField(CajetaModulePtr module, string name, CajetaTypePtr type, bool reference, set<Modifier> modifiers, set<QualifiedNamePtr> annotations,
            InitializerPtr initializer);

        // Loads the slot's current value, materializing the slot first if this is the
        // first access. A value-semantics type loads at its own type; every other
        // type is slot-held as a `ptr` and loads as one.
        llvm::Value* createLoad() override;
        // Stores `value` into the slot, materializing the slot first if this is the
        // first access. Returns the store instruction.
        llvm::Value* createStore(llvm::Value* value) override;
        llvm::AllocaInst* getOrCreateAllocation() override;
    };

} // code