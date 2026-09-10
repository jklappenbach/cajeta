// Task<T> layout, drop wrapper and per-T cache; see CajetaTask.h for the design.

#include "CajetaTask.h"
#include "CajetaArray.h"
#include "CajetaView.h"
#include "../compile/CajetaModule.h"
#include "../compile/CompilationContext.h"
#include "llvm/TargetParser/Triple.h"

namespace cajeta {

    llvm::Type* CajetaTask::getLlvmType() {
        if (isFrozen() && CajetaType::rawLlvmType() == nullptr) {
            llvm::LLVMContext* ctx = currentLlvmContext();
            if (!ctx && module) ctx = module->getLlvmContext();
            if (ctx) setLlvmType(buildLlvmType(ctx));
        }
        return CajetaClass::getLlvmType();
    }

    CajetaTask::CajetaTask(CajetaModulePtr module, CajetaTypePtr elementType)
        : CajetaClass(module) {
        this->elementType = elementType;
        string typeName = string("Task<") + elementType->toCanonical() + ">";
        // Unqualified by design: Task is compiler-synthesized, not the reserved
        // `cajeta.concurrent.Task<T>` package name.
        qName = QualifiedName::getOrCreate(typeName);
        canonical = qName->toCanonical();
        // Without typeArguments the unifier reads Task<R> as non-instantiated
        // and silently fails to bind R from a Task<int32> argument.
        setTypeArguments({elementType});

        setLlvmType(buildLlvmType(module->getLlvmContext()));
        typeFlags = STRUCT_FLAG | USER_DEFINED_FLAG;
    }

    llvm::Type* CajetaTask::buildLlvmType(llvm::LLVMContext* ctx) const {
        llvm::Type* valueLlvm;
        bool isStruct = dynamic_pointer_cast<CajetaView>(elementType) != nullptr;
        bool isArr = dynamic_pointer_cast<CajetaArray>(elementType) != nullptr;
        bool isClassLike = dynamic_pointer_cast<CajetaClass>(elementType) != nullptr;
        bool isPrim = elementType && (elementType->getTypeFlags() & PRIMITIVE_FLAG);
        bool storeAsPtr = (isClassLike && !isStruct) && (isArr || !isPrim);
        // LLVM has no void struct member, so Task<void>'s value slot is a dead
        // i8; SpawnExpression skips the store and await never reads it.
        bool isVoid = elementType && elementType->getLlvmType()
            && elementType->getLlvmType()->isVoidTy();
        if (isVoid) {
            valueLlvm = llvm::Type::getInt8Ty(*ctx);
        } else if (storeAsPtr) {
            valueLlvm = llvm::PointerType::get(*ctx, 0);
        } else {
            valueLlvm = elementType->getLlvmType();
        }

        // { T value, i32 done, ptr exception, ptr fiber }: `done` is i32 for the
        // runtime's atomic store, `exception` is the Throwable* the trampoline
        // writes, `fiber` is the cajeta_fiber* __cajeta_task_run allocates.
        vector<llvm::Type*> fields = {
            valueLlvm,
            llvm::Type::getInt32Ty(*ctx),
            llvm::PointerType::get(*ctx, 0),
            llvm::PointerType::get(*ctx, 0),
        };
        return CajetaType::getOrCreateLlvmType(ctx,
            string("#task.") + canonical, fields);
    }

    llvm::Function* CajetaTask::getOrCreateDropFunction() {
        auto& llvmDropFunction = dropFnRef();
        if (llvmDropFunction) return llvmDropFunction;
        auto& ctx = *module->getLlvmContext();
        auto* lmod = module->getLlvmModule();
        llvm::PointerType* ptrTy = llvm::PointerType::get(ctx, 0);
        llvm::FunctionType* fnTy = llvm::FunctionType::get(
            llvm::Type::getVoidTy(ctx), {(llvm::Type*) ptrTy}, false);

        // `Task<...>` is not a C identifier, so the canonical is sanitized and
        // prefixed to keep the symbol clear of a user class named `Task`.
        string dropName = string("__cajeta_task_") + canonical + "_drop";
        for (char& c : dropName) {
            if (c == ':' || c == '.' || c == '<' || c == '>'
                    || c == ',' || c == ' ') {
                c = '_';
            }
        }

        if (llvm::Function* existing = lmod->getFunction(dropName)) {
            llvmDropFunction = existing;
            return existing;
        }

        // LinkOnceODR merges the per-T body across JIT modules instead of
        // "symbol multiply defined".
        llvmDropFunction = llvm::Function::Create(fnTy,
            llvm::Function::LinkOnceODRLinkage, dropName, lmod);
        // The COMDAT only helps ELF/COFF dedupe; LLVM's MachO writer aborts on
        // one, and LinkOnceODR alone already carries the merge semantics.
        llvm::Triple lmodTriple(lmod->getTargetTriple());
        if (!lmodTriple.isOSBinFormatMachO()) {
            llvmDropFunction->setComdat(lmod->getOrInsertComdat(dropName));
        }
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
        // On the throw path __cajeta_throw fires drops without scope_exit_to, so
        // this wait is the only thing keeping the free off a live worker struct.
        llvm::Function* waitFn = module->getRuntimeFunction("__cajeta_task_wait");
        if (waitFn) {
            llvm::Value* doneAddr = b.CreateStructGEP(
                rawLlvmType(), task, DONE_FIELD_INDEX, "task_done");
            b.CreateCall(waitFn, {doneAddr});
        }
        // Deregister before the free: scope_exit_to runs after the drop chain and
        // would otherwise walk scope_register entries into freed memory.
        llvm::Function* deregFn = module->getRuntimeFunction(
            "__cajeta_scope_deregister_task");
        if (deregFn) {
            const llvm::DataLayout& dl = lmod->getDataLayout();
            uint64_t taskSize = dl.getTypeAllocSize(rawLlvmType());
            llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
            b.CreateCall(deregFn, {task,
                llvm::ConstantInt::get(i64Ty, taskSize)});
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
