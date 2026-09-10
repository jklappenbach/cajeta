// HeapField - a Field whose value lives in heap storage.

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

        // The entry-block pointer slot, created once and initialized on that first
        // call, so a `heap` local inside a loop does not re-allocate. Null for a
        // NAME-ONLY binding, which session scope seeds when the type does not resolve.
        llvm::AllocaInst* getOrCreateAllocation() override;

        // Emits the free of the pointed-to allocation at the current insert point.
        // A block that already has a terminator is skipped: a free after `ret`
        // would leave dangling instructions and fail LLVM verification.
        void onDelete() override;
    };
}