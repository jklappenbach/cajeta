//
// Created by James Klappenbach on 2/20/22.
//

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
        // `reference` is inherited from Field — do NOT redeclare it here, or the
        // shadow hides Field::reference and isReference() never sees writes made
        // through ParameterField.
        llvm::Function* llvmFunction;
        int paramIndex;
        // Retained reference to the declaring formal so downstream
        // borrow-escape checks (Phase 3 of #68, body-side `#T` contract
        // enforcement) can consult the formal's `transferred` bit
        // without re-walking the method's parameter list.
        FormalParameterPtr formalParameter;

    public:
        ParameterField(CajetaModulePtr module, FormalParameterPtr formalParameter, llvm::Function* llvmFunction, int paramIndex);

        FormalParameterPtr getFormalParameter() const { return formalParameter; }

        llvm::Value* createStore(llvm::Value* value) override;

        llvm::Value* createLoad() override;

        llvm::AllocaInst* getOrCreateAllocation() override;
    };
}