// Cajeta's async-task wrapper: `async T fn(...)` returns a heap `Task<T>` laid out as
// { value, done, exception, fiber }. A CajetaClass-derived wrapper outside the user's
// declaration space, instantiated lazily per-T and cached on the module's struct map.
#pragma once

#include "CajetaClass.h"

namespace cajeta {
    class CajetaTask : public CajetaClass {
    private:
        CajetaTypePtr elementType;
    public:
        // Field indices in the heap struct's GEP layout.
        static constexpr unsigned VALUE_FIELD_INDEX = 0;
        static constexpr unsigned DONE_FIELD_INDEX = 1;
        // Throwable* the fiber trampoline writes when the inner async fn throws; NULL
        // on the success path, and await re-raises a non-null one into its own frame.
        static constexpr unsigned EXCEPTION_FIELD_INDEX = 2;
        // The fiber running this task, written by __cajeta_task_run after allocating it
        // and NULL while queued. Scope cancels remaining children through it on a throw.
        static constexpr unsigned FIBER_FIELD_INDEX = 3;

        CajetaTask(CajetaModulePtr module, CajetaTypePtr elementType);

        CajetaTypePtr getElementType() const { return elementType; }

        // Build (intern) the Task<T> struct in `ctx`, for the per-thread rebuild.
        llvm::Type* buildLlvmType(llvm::LLVMContext* ctx) const;

        // Frozen per-thread rebuild on an empty table; inert while not frozen.
        llvm::Type* getLlvmType() override;

        // Get or create the Task<T> wrapper for `elementType`, cached on the module.
        static shared_ptr<CajetaTask> getOrCreate(CajetaModulePtr module,
                                                  CajetaTypePtr elementType);

        // The default class drop just frees, which races a task the worker fiber has
        // not finished; Task's drop waits for `done` first.
        llvm::Function* getOrCreateDropFunction() override;

        // Task<T>'s layout carries no vtable pointer at slot 0, so the virtual-drop
        // dispatcher would misread slot 0 and segfault. Task is monomorphic.
        bool hasVtablePointerAtSlotZero() const override { return false; }
    };
    typedef shared_ptr<CajetaTask> CajetaTaskPtr;
}
