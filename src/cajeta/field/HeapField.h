//
// Created by James Klappenbach on 2/20/22.
//

#pragma once

#include "set"
#include "list"
#include "../type/QualifiedName.h"
#include "../type/Modifiable.h"
#include "../type/Annotatable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/IRBuilder.h"
#include "../util/MemoryManager.h"
#include "Field.h"

using namespace std;

namespace cajeta {
    class HeapField : public Field, public enable_shared_from_this<Field> {
    protected:

        string buildHierarchicalName() override {
            return name;
        }

    public:
        HeapField(CajetaModulePtr module, string name, CajetaTypePtr type) : Field(module, name, type) {

        }

        HeapField(CajetaModulePtr module, string& name, CajetaTypePtr type, llvm::AllocaInst* alloca) : Field(module, name, type,
            alloca) {
        }

        HeapField(CajetaModulePtr module, string name, CajetaTypePtr type, bool reference, set<Modifier> modifiers,
            set<QualifiedNamePtr> annotations, InitializerPtr initializer) : Field(module, name, type, reference, modifiers,
            annotations, initializer) {
        }

        llvm::Value* createLoad() override;
        llvm::Value* createStore(llvm::Value* value) override;

        llvm::AllocaInst* getOrCreateAllocation() override;

        void onDelete() override;
    };
}