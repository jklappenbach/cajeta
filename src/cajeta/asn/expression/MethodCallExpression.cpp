//
// Created by James Klappenbach on 4/19/23.
//

#include "MethodCallExpression.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaArray.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/method/Method.h"
#include "Expression.h"

namespace cajeta {

    // Codegen dispatches three shapes:
    //   1. `arr.size()` on a CajetaArray receiver — structural accessor, loads the i64
    //      size field from the array header. Same shape will apply to every collection
    //      type per the project memory ("collections expose size() returning int64").
    //   2. `obj.foo(args)` with a class receiver — invokeMethod on the receiver's type.
    //   3. Bare `foo(args)` — resolves on the enclosing class with `this` as receiver.
    llvm::Value* MethodCallExpression::generateCode(CajetaModulePtr module) {
        auto* builder = module->getBuilder();

        // Determine receiver (if any) from children[0]; the lhs is added when this node
        // was constructed via the DOT-methodCall branch.
        llvm::Value* receiver = nullptr;
        CajetaTypePtr receiverType;
        if (!children.empty()) {
            receiver = children[0]->generateCode(module);
            auto exprChild = dynamic_pointer_cast<Expression>(children[0]);
            if (exprChild) {
                if (!exprChild->getResolvedType()) {
                    exprChild->resolveTypes(module);
                }
                receiverType = exprChild->getResolvedType();
            }
            // l-value -> r-value coercion. Local-variable receivers are AllocaInsts;
            // ArrayIndex receivers are slot addresses where the slot holds a `ptr` to
            // the referenced object (CajetaArray inner header or class instance).
            if (auto* a = llvm::dyn_cast<llvm::AllocaInst>(receiver)) {
                receiver = builder->CreateLoad(a->getAllocatedType(), a);
            } else if (dynamic_pointer_cast<ArrayIndexExpression>(exprChild)) {
                receiver = builder->CreateLoad(
                    llvm::PointerType::get(*module->getLlvmContext(), 0), receiver);
            }
        }

        // Structural accessors on arrays: size() reads the header's first field.
        if (receiver && methodCallName == "size") {
            if (auto arrayType = dynamic_pointer_cast<CajetaArray>(receiverType)) {
                llvm::Value* sizePtr = builder->CreateStructGEP(
                    arrayType->getLlvmType(), receiver, CajetaArray::SIZE_FIELD_INDEX);
                return builder->CreateLoad(
                    llvm::Type::getInt64Ty(*module->getLlvmContext()), sizePtr);
            }
        }

        // Resolve the target class either from the receiver (cross-object call) or from
        // the enclosing class on the structure stack (bare call).
        CajetaClassPtr targetClass;
        if (auto klass = dynamic_pointer_cast<CajetaClass>(receiverType)) {
            targetClass = klass;
        }
        if (!targetClass) {
            if (module->getStructureStack().empty()) {
                return nullptr;
            }
            targetClass = module->getStructureStack().back();
        }

        // Resolve `this`. For cross-object calls the receiver IS the `this`. For bare
        // calls we look it up from the active method's scope.
        llvm::Value* thisValue = receiver;
        if (!thisValue) {
            FieldPtr thisField = module->getScopeStack().peek()->getField("this");
            if (thisField) {
                llvm::AllocaInst* thisAlloca = thisField->getOrCreateAllocation();
                thisValue = builder->CreateLoad(thisAlloca->getAllocatedType(), thisAlloca);
            }
        }

        // Evaluate args, loading any l-values.
        vector<ParameterEntry> entries;
        for (auto& param : parameters) {
            llvm::Value* value = param.expression->generateCode(module);
            if (auto* a = llvm::dyn_cast<llvm::AllocaInst>(value)) {
                value = builder->CreateLoad(a->getAllocatedType(), a);
            }
            entries.push_back(ParameterEntry(CajetaType::of(value), param.label, value));
        }

        return targetClass->invokeMethod(methodCallName, entries, /*isConstructor=*/false, thisValue);
    }


} // code
