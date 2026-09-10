// BoundClosureField — a function-typed scope entry bound to a statically-known
// function: an invocation `P(args)` lowers to a direct call, while a forwarded P
// reads as the lambda's constant closure record `{ fn, null, null }`.

#pragma once

#include "Field.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Constant.h"

namespace cajeta {

    class BoundClosureField : public Field {
        llvm::Function* boundFunction;   // direct-call target (invocation sites)
        llvm::Constant* closureRecord;   // { fn, null, null } global (forwarded reads)
        llvm::AllocaInst* slot = nullptr;
    public:
        BoundClosureField(CajetaModulePtr module, string name, CajetaTypePtr fnType,
                          llvm::Function* boundFunction, llvm::Constant* closureRecord)
            : Field(module, std::move(name), std::move(fnType)),
              boundFunction(boundFunction), closureRecord(closureRecord) {}

        llvm::Function* getBoundFunction() const { return boundFunction; }

        // A forwarded read yields the constant closure record (slot-backed, so it
        // composes with the existing l-value machinery). Invocation sites never
        // reach these — they take the direct path in MethodCallExpression.
        llvm::Value* createLoad() override;
        // Materializes the slot once: an entry alloca whose store of the closure
        // record is emitted immediately after it, so the record dominates every
        // forwarded use. Null when there is no record (an invocation-only bind).
        llvm::AllocaInst* getOrCreateAllocation() override;
        llvm::Value* createStore(llvm::Value*) override { return nullptr; }
    };

}
