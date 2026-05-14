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
        // Void-returning async functions produce a Task<void>; LLVM
        // doesn't allow void inside a struct, so substitute i8 as a
        // dead placeholder at the value slot. SpawnExpression's
        // value-store path detects the void case and skips the store
        // entirely — the slot is never read either (await on void
        // returns no value).
        bool isVoid = elementType && elementType->getLlvmType()
            && elementType->getLlvmType()->isVoidTy();
        if (isVoid) {
            valueLlvm = llvm::Type::getInt8Ty(*ctx);
        } else if (storeAsPtr) {
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

    llvm::Function* CajetaTask::getOrCreateDropFunction() {
        if (llvmDropFunction) return llvmDropFunction;
        auto& ctx = *module->getLlvmContext();
        auto* lmod = module->getLlvmModule();
        llvm::PointerType* ptrTy = llvm::PointerType::get(ctx, 0);
        llvm::FunctionType* fnTy = llvm::FunctionType::get(
            llvm::Type::getVoidTy(ctx), {(llvm::Type*) ptrTy}, false);

        // Sanitize the canonical to a valid C identifier so the symbol
        // reads sensibly in stack traces — same convention CajetaClass
        // uses, but prefixed `__cajeta_task_` to distinguish from
        // regular class drop wrappers and to avoid colliding with a
        // user class literally named `Task` (the synthesized type's
        // canonical is `Task<...>` which includes angle brackets).
        string dropName = string("__cajeta_task_") + canonical + "_drop";
        for (char& c : dropName) {
            if (c == ':' || c == '.' || c == '<' || c == '>'
                    || c == ',' || c == ' ') {
                c = '_';
            }
        }

        // Reuse if the JIT module has built this drop fn before
        // (Task<T> for the same T can be referenced from multiple
        // spawn sites within one module).
        if (llvm::Function* existing = lmod->getFunction(dropName)) {
            llvmDropFunction = existing;
            return existing;
        }

        llvmDropFunction = llvm::Function::Create(fnTy,
            llvm::Function::ExternalLinkage, dropName, lmod);
        llvm::BasicBlock* bb = llvm::BasicBlock::Create(
            ctx, "entry", llvmDropFunction);
        llvm::IRBuilder<> b(bb);
        llvm::Value* task = llvmDropFunction->getArg(0);

        llvm::BasicBlock* doDrop = llvm::BasicBlock::Create(
            ctx, "doDrop", llvmDropFunction);
        llvm::BasicBlock* done = llvm::BasicBlock::Create(
            ctx, "done", llvmDropFunction);
        llvm::Value* isNull = b.CreateICmpEQ(task,
            llvm::ConstantPointerNull::get(ptrTy));
        b.CreateCondBr(isNull, done, doDrop);

        b.SetInsertPoint(doDrop);
        // Wait for completion before freeing. On the normal fall-through
        // path Method::generateCode runs __cajeta_scope_exit_to BEFORE
        // emitOwnerDrops, so by the time this fires the task is already
        // done and wait is a no-op atomic load. On the throw path, the
        // unwind in __cajeta_throw fires drops directly (line 1078 of
        // cajeta_runtime.c) — scope_exit_to doesn't run, so the wait
        // here is what keeps the carrier from freeing a struct the
        // worker still touches.
        llvm::Function* waitFn = module->getRuntimeFunction("__cajeta_task_wait");
        if (waitFn) {
            llvm::Value* doneAddr = b.CreateStructGEP(
                llvmType, task, DONE_FIELD_INDEX, "task_done");
            b.CreateCall(waitFn, {doneAddr});
        }
        llvm::Function* freeFn = module->getRuntimeFunction("__cajeta_free");
        if (freeFn) {
            b.CreateCall(freeFn, {task});
        }
        b.CreateBr(done);
        b.SetInsertPoint(done);
        b.CreateRetVoid();
        return llvmDropFunction;
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
