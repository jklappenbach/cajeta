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

        CajetaTask(CajetaModulePtr module, CajetaTypePtr elementType);

        CajetaTypePtr getElementType() const { return elementType; }

        // Get or create the Task<T> wrapper for the given element type,
        // caching the instance on the module so every Task<T> reference for
        // the same T resolves to the same CajetaClass.
        static shared_ptr<CajetaTask> getOrCreate(CajetaModulePtr module,
                                                  CajetaTypePtr elementType);
    };
    typedef shared_ptr<CajetaTask> CajetaTaskPtr;
}
