//
// BoundClosureField — a function-typed scope entry bound to a statically-known
// function (cajeta-ir Unit 4, Approach A).
//
// When the closure-specialization factory materializes a specialized method
// instance F$<fn>, the function-typed parameter P is dropped from the signature
// and replaced, in the instance's body scope, by one of these: the name P now
// resolves to a known llvm::Function (the non-capturing lambda's synthesized
// fn). A call `P(args)` therefore lowers to a DIRECT call (emitClosureCall with
// a known target) — the front-end realization of the comparator devirtualization.
//
// P may also be FORWARDED (passed on to another call) rather than invoked — e.g.
// Sort.binarySearch hands its comparator to lowerBound. For that the field still
// behaves like an ordinary closure value: reads yield the lambda's constant
// closure RECORD `{ fn, null, null }`, materialized through a real slot, so a
// forwarded comparator dispatches correctly (indirect on a constant) while
// direct invocations remain direct. This keeps specialization SOUND regardless
// of whether the parameter escapes — no escape gate needed.
//

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
        llvm::AllocaInst* getOrCreateAllocation() override;
        llvm::Value* createStore(llvm::Value*) override { return nullptr; }
    };

}
