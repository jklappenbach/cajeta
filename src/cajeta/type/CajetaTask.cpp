//
// See CajetaTask.h for the design.
//

#include "CajetaTask.h"
#include "CajetaArray.h"
#include "CajetaStruct.h"
#include "../compile/CajetaModule.h"

namespace cajeta {

    CajetaTask::CajetaTask(CajetaModulePtr module, CajetaTypePtr elementType)
        : CajetaClass(module) {
        this->elementType = elementType;
        string typeName = string("Task<") + elementType->toCanonical() + ">";
        // No package prefix — Task is a built-in compiler-synthesized type,
        // distinct from user-declared `cajeta.threading.Task<T>` (which is
        // the package the doc reserves for it but doesn't exist as a real
        // class today).
        qName = QualifiedName::getOrCreate(typeName);
        canonical = qName->toCanonical();

        llvm::LLVMContext* ctx = module->getLlvmContext();

        // Element-storage type: classes/arrays travel as `ptr` (heap-allocated,
        // pass-by-reference at the LLVM level). Primitives store their LLVM
        // type directly.
        llvm::Type* valueLlvm;
        bool isStruct = dynamic_pointer_cast<CajetaStruct>(elementType) != nullptr;
        bool isArr = dynamic_pointer_cast<CajetaArray>(elementType) != nullptr;
        bool isClassLike = dynamic_pointer_cast<CajetaClass>(elementType) != nullptr;
        bool isPrim = elementType && (elementType->getTypeFlags() & PRIMITIVE_FLAG);
        bool storeAsPtr = (isClassLike && !isStruct) && (isArr || !isPrim);
        if (storeAsPtr) {
            valueLlvm = llvm::PointerType::get(*ctx, 0);
        } else {
            valueLlvm = elementType->getLlvmType();
        }

        // Layout: { T value, i32 done, ptr exception, ptr fiber }. `done`
        // is i32 so the C runtime can atomic-store it. `exception` is the
        // Throwable* the trampoline writes on throw. `fiber` is the
        // cajeta_fiber* the runtime allocates inside __cajeta_task_run;
        // scope uses it for R5-C cancellation (set the fiber's cancel_with
        // so its next await aborts).
        vector<llvm::Type*> fields = {
            valueLlvm,
            llvm::Type::getInt32Ty(*ctx),
            llvm::PointerType::get(*ctx, 0),
            llvm::PointerType::get(*ctx, 0),
        };
        llvmType = CajetaType::getOrCreateLlvmType(ctx,
            string("#task.") + canonical, fields);
        typeFlags = STRUCT_FLAG | USER_DEFINED_FLAG;
    }

    shared_ptr<CajetaTask> CajetaTask::getOrCreate(CajetaModulePtr module,
                                                    CajetaTypePtr elementType) {
        string key = string("Task<") + elementType->toCanonical() + ">";
        // Module structure map is the canonical cache for synthesized types.
        // Look up the existing instance to avoid duplicating layouts (the
        // LLVM struct type would otherwise multiply).
        auto& structures = module->getStructures();
        auto it = structures.find(key);
        if (it != structures.end()) {
            if (auto task = dynamic_pointer_cast<CajetaTask>(it->second)) {
                return task;
            }
        }
        auto task = make_shared<CajetaTask>(module, elementType);
        structures[task->toCanonical()] = task;
        return task;
    }
}
