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
        bool reference;
        llvm::Function* llvmFunction;
        int paramIndex;

    public:
        ParameterField(CajetaModulePtr module, FormalParameterPtr formalParameter, llvm::Function* llvmFunction, int paramIndex);

        llvm::Value* createStore(llvm::Value* value) override;

        llvm::Value* createLoad() override;

        llvm::AllocaInst* getOrCreateAllocation() override;
    };
}