//
// Created by James Klappenbach on 11/4/22.
//

#include "LocalVariableDeclaration.h"
#include "VariableDeclarator.h"
#include "../compile/CajetaModule.h"
#include "../field/HeapField.h"
#include "../field/StackField.h"
#include "../type/CajetaArray.h"
#include "../type/CajetaFunctionType.h"
#include "expression/Expression.h"
#include "../method/Method.h"
#include "../error/CajetaExceptions.h"
#include "../logging/CajetaLogger.h"

namespace cajeta {
    // Size in bytes of the runtime's cajeta_drop_entry struct on the target
    // platform. The runtime exposes __cajeta_drop_entry_size() if we ever need
    // to validate this; for x86-64 and aarch64 the struct fits in 32 bytes
    // (8 byte obj ptr + 8 byte drop fn ptr + 8 byte prev ptr + 1 byte active +
    // padding).
    static constexpr unsigned DROP_ENTRY_BYTES = 32;

    // Emit drop-chain wiring for an owner local. Allocates a DropEntry blob on
    // the stack at function entry, pushes it onto the runtime's chain right
    // after the owner is materialized, and records the entry on both the field
    // and the enclosing method so scope-exit emits the matching pop.
    static void emitDropEntryFor(CajetaModulePtr module, FieldPtr field,
                                  const std::string& dropFnName) {
        llvm::Function* push = module->getRuntimeFunction("__cajeta_drop_push");
        llvm::Function* dropFn = module->getRuntimeFunction(dropFnName);
        if (!push || !dropFn) return;
        auto* builder = module->getBuilder();
        auto& ctx = *module->getLlvmContext();
        llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);
        llvm::Type* ptrTy = llvm::PointerType::get(ctx, 0);

        // Allocate the DropEntry in the function's entry block so its address
        // is stable across the function's lifetime (matters because the chain
        // threads pointers through it).
        llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
        llvm::IRBuilder<> entryBuilder(&parentFn->getEntryBlock(),
            parentFn->getEntryBlock().begin());
        llvm::Value* entryPtr = entryBuilder.CreateAlloca(
            llvm::ArrayType::get(i8Ty, DROP_ENTRY_BYTES));

        // Load the owner pointer to pass as the drop function's `obj` arg.
        llvm::Value* ownerPtr = builder->CreateLoad(ptrTy, field->getOrCreateAllocation());
        builder->CreateCall(push, {entryPtr, ownerPtr, dropFn});

        field->setDropEntry(entryPtr);
        if (auto m = module->getCurrentMethod()) m->registerDropEntry(entryPtr);
    }

    /**
     * If we have a primitive variable, we can store in on the stack and will immediately create an currentRegister.
     * Otherwise, we will create an currentRegister for a structure reference.  If the variable receives a new operator,
     * we'll just let the malloc call create the register
     *
     * @param module
     * @return
     */
    llvm::Value* LocalVariableDeclaration::generateCode(CajetaModulePtr module) {

        // Arrays and class instances live on the heap; their local slot is a pointer.
        // Only true primitives (int32, float64, bool, etc.) get an inline-value alloca.
        bool isArray = dynamic_pointer_cast<CajetaArray>(type) != nullptr;
        bool wantsInlineSlot = (type->getTypeFlags() & PRIMITIVE_FLAG) && !isArray;
        for (auto& declarator: variableDeclarators) {
            InitializerPtr initializer = declarator->getInitializer();
            // Array-literal initializer (`int32[] xs = {1, 2, 3}`): the
            // literal has no type of its own, so push the element type
            // down here before codegen so the literal knows how big the
            // slots are and how to coerce its values.
            if (isArray) {
                if (auto arrInit = dynamic_pointer_cast<ArrayInitializer>(initializer)) {
                    if (auto arrType = dynamic_pointer_cast<CajetaArray>(type)) {
                        arrInit->setElementType(arrType->getElementType());
                    }
                }
            }
            // Function-typed initializer with a lambda RHS: push the LHS's
            // function type down to the lambda so it can use the declared
            // return type (and, eventually, expected param types) rather
            // than trying to infer them from a body whose own resolvedType
            // isn't always populated. See cajeta-docs/Lambdas.md.
            if (auto varInit = dynamic_pointer_cast<VariableInitializer>(initializer)) {
                auto& children = varInit->getChildren();
                if (!children.empty()) {
                    if (auto lambda = dynamic_pointer_cast<LambdaExpression>(children[0])) {
                        lambda->setExpectedType(type);
                    }
                }
            }
            FieldPtr field;
            if (wantsInlineSlot) {
                field = make_shared<StackField>(module, declarator->getIdentifier(), type,
                    declarator->isReference(), modifiers, annotations, initializer);
            } else {
                field = make_shared<HeapField>(module, declarator->getIdentifier(), type,
                    declarator->isReference(), modifiers, annotations, initializer);
            }
            module->getScopeStack().peek()->putField(field);
            field->getOrCreateAllocation();

            // L3-2 escape-check wiring: a function-typed local initialized
            // from a lambda inherits the lambda's borrow-capture state.
            // After the initializer has run (putField → getOrCreateAllocation
            // triggered the lambda's generateCode and its capture analysis),
            // copy the flag onto the field so a later `return fnLocal` can
            // surface the dangling-borrow error before LLVM verify.
            if (auto varInit = dynamic_pointer_cast<VariableInitializer>(initializer)) {
                auto& children = varInit->getChildren();
                if (!children.empty()) {
                    if (auto lambda = dynamic_pointer_cast<LambdaExpression>(children[0])) {
                        if (lambda->getHasBorrowCaptures()) {
                            field->setHasBorrowCaptures(true);
                        }
                    }
                }
            }

            // Wire the drop chain for owned heap allocations. v1 covers
            // CajetaArray locals — the most concrete case where we already have
            // a runtime free function. Class instances and String-owning locals
            // join later as their drop semantics get pinned.
            if (isArray) {
                emitDropEntryFor(module, field, "__cajeta_free_array");
            }

            // L3-3: function-typed locals own the closure record they
            // point at (for capturing closures) and need a drop entry
            // that fires __cajeta_closure_drop at scope exit. Non-
            // capturing closures store a stack-allocated record with
            // drop_fn=null, so the runtime helper no-ops on them; the
            // entry shape is therefore safe for every function-typed
            // local regardless of what it holds. ReturnStatement
            // deactivates the entry when the local is returned so
            // ownership transfers to the caller without a double-free.
            if (dynamic_pointer_cast<CajetaFunctionType>(type)) {
                emitDropEntryFor(module, field, "__cajeta_closure_drop");
            }
        }

        return nullptr;
    }

} // code