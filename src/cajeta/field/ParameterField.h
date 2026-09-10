// Created by James Klappenbach on 2/20/22.

#pragma once

#include <set>
#include <list>
#include "cajeta/type/QualifiedName.h"
#include "cajeta/type/Modifiable.h"
#include "cajeta/type/Annotatable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include <llvm/IR/IRBuilder.h>
#include "cajeta/util/MemoryManager.h"
#include "cajeta/type/FormalParameter.h"

using namespace std;

namespace cajeta {
    class CajetaModule;
    typedef shared_ptr<CajetaModule> CajetaModulePtr;

    class ParameterField : public Field, public enable_shared_from_this<Field> {
    protected:
        // `reference` is inherited from Field — do NOT redeclare it here, or the shadow
        // hides Field::reference and isReference() never sees writes made through this.
        llvm::Function* llvmFunction;
        int paramIndex;
        // The declaring formal is retained so borrow-escape checks can read its `transferred` bit without re-walking the parameter list.
        FormalParameterPtr formalParameter;

    public:
        ParameterField(CajetaModulePtr module, FormalParameterPtr formalParameter, llvm::Function* llvmFunction, int paramIndex);

        FormalParameterPtr getFormalParameter() const { return formalParameter; }

        // Stores `value` into the parameter's slot, materializing the slot first if
        // this is the first access. Returns the store instruction.
        llvm::Value* createStore(llvm::Value* value) override;

        // Loads the parameter's current value, materializing the slot first if this
        // is the first access. A @ValueType slot holds the aggregate inline and is
        // loaded at its own type; every reference type loads as a `ptr`.
        llvm::Value* createLoad() override;

        // The entry-block slot holding the incoming argument, created and stored into
        // on first use, so a first reference inside a loop does not re-allocate. Throws
        // CAJETA_ERROR_PROTOTYPE_ABI_MISMATCH when paramIndex exceeds the prototype.
        llvm::AllocaInst* getOrCreateAllocation() override;
    };
}