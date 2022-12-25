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
#include "cajeta/field/LocalField.h"

using namespace std;

namespace cajeta {
    class LocalStructureField : public LocalField {
    protected:

        string buildHierarchicalName() override {
            return name;
        }

    public:
        LocalStructureField(string name, CajetaType* type) : LocalField(name, type) { }

        LocalStructureField(string& name, CajetaType* type, llvm::Value* allocation) : LocalField(name, type, allocation) {
        }

        LocalStructureField(string name, CajetaType* type, bool reference, set<Modifier> modifiers,
            set<QualifiedName*> annotations, Initializer* initializer) : LocalField(name, type, reference, modifiers, annotations, initializer) {

        }

        llvm::Value* createLoad(CajetaModule* module) override;

        llvm::Value* getOrCreateAllocation(CajetaModule* module) override;

        void onDelete(CajetaModule* module, Scope* scope) override;
    };
}