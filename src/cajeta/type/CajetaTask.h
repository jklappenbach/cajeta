//
// Cajeta's async-task wrapper. `async T fn(...)` returns a heap-allocated
// `Task<T>` whose layout is `{ T value, i1 done }`. The spawn/await pair
// materializes and unwraps these instances respectively.
//
// In the R1 milestone (this file's introduction), the runtime is still
// synchronous — `done` is set true at the moment of allocation, no
// scheduler is involved, and `await` is just a value-load. R2+ extends
// the same struct with continuation pointers + a wait queue without
// breaking the surface ABI.
//
// Modeled on CajetaArray: it's a CajetaClass-derived wrapper that lives
// outside the user's class declaration space. Instantiated lazily per-T
// at first use; cached on the module's structure map so all references
// to `Task<T>` for a given T see the same instance.
//

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
        // R5/Error-model #205: Throwable* slot the fiber trampoline writes
        // when the inner async fn throws. NULL on the success path. await
        // checks this slot after task_wait — non-null means re-raise into
        // the awaiter's frame.
        static constexpr unsigned EXCEPTION_FIELD_INDEX = 2;
        // R5-C: ptr to the fiber that runs this task. __cajeta_task_run
        // writes this after allocating the fiber. Scope uses it to call
        // __cajeta_fiber_cancel on remaining children when one throws.
        // NULL while the task is queued but not yet popped by the carrier.
        static constexpr unsigned FIBER_FIELD_INDEX = 3;

        CajetaTask(CajetaModulePtr module, CajetaTypePtr elementType);

        CajetaTypePtr getElementType() const { return elementType; }

        // U6.4.1 — build (intern) the Task<T> struct in `ctx` from the element
        // type. Context-parameterized for frozen-stdlib per-thread rebuild
        // (U6.4.2); the ctor calls it with the home module's context.
        llvm::Type* buildLlvmType(llvm::LLVMContext* ctx) const;

        // U6.4.2 — frozen per-thread rebuild on empty table; inert while not frozen.
        llvm::Type* getLlvmType() override;

        // Get or create the Task<T> wrapper for the given element type,
        // caching the instance on the module so every Task<T> reference for
        // the same T resolves to the same CajetaClass.
        static shared_ptr<CajetaTask> getOrCreate(CajetaModulePtr module,
                                                  CajetaTypePtr elementType);

        // Override: the default CajetaClass drop wrapper just frees, which
        // races the worker fiber if the task hasn't completed yet. Task's
        // drop must wait for `done` before freeing. SpawnExpression pushes
        // a drop entry that calls this wrapper, so every spawned Task gets
        // reclaimed at scope exit (or during exception unwind), eliminating
        // the leak documented in AsyncStatus.md § Known gaps.
        llvm::Function* getOrCreateDropFunction() override;

        // Task<T>'s instance layout is { fn, arg, done, ... } — no
        // vtable pointer at slot 0. The virtual-drop dispatcher would
        // misread the function pointer at slot 0 as a vtable and
        // segfault on the drop_fn lookup. Static dispatch through
        // getOrCreateDropFunction (overridden above) is both correct
        // and sufficient: Task is monomorphic, no subclassing exists.
        bool hasVtablePointerAtSlotZero() const override { return false; }
    };
    typedef shared_ptr<CajetaTask> CajetaTaskPtr;
}
