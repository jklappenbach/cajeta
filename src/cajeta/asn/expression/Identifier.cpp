//
// Created by James Klappenbach on 4/14/23.
//

#include "Identifier.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/error/Exception.h"
#include "../../type/CajetaClass.h"
#include "../../type/StructureProperty.h"

namespace cajeta {
    void IdentifierExpression::resolveTypes(CajetaModulePtr module) {
        // Look up the identifier in the active scope and pin our resolvedType to the
        // referenced field's type. Used downstream by DotExpression and ArrayIndexExpression.
        if (!module->getScopeStack().isEmpty()) {
            FieldPtr field = module->getScopeStack().peek()->getField(identifier);
            if (field) {
                resolvedType = field->getType();
                return;
            }
        }
        // Implicit-this fallback: a bare identifier inside an instance
        // method body might be naming a property of the enclosing class
        // (the common `x` shorthand for `this.x`). Look up against the
        // class on the top of the structure stack.
        if (!module->getStructureStack().empty()) {
            auto klass = module->getStructureStack().back();
            if (klass) {
                auto it = klass->getProperties().find(identifier);
                if (it != klass->getProperties().end()) {
                    resolvedType = it->second->getType();
                }
            }
        }
        // Intentionally do NOT resolve class-name identifiers here.
        // MethodReferenceExpression's resolver distinguishes a value-of-
        // class-type receiver (`myInstance::next` → BOUND) from a
        // class-name receiver (`Counter::next` → UNBOUND) using whether
        // resolvedType is non-null on the LHS expression. Pinning a class
        // type on the LHS for bare-class-name lookups would collapse the
        // distinction and break that discriminator. Static-method calls
        // (`Bar.work()`) handle the class-name fallback in
        // MethodCallExpression directly.
    }

    llvm::Value* IdentifierExpression::generateCode(CajetaModulePtr module) {
        // A transferred binding is READABLE. `#` moves the title, not the
        // binding: the source is demoted to a borrow of the same live
        // instance, and borrows are readable. Transferring AGAIN is what is
        // rejected, and that check lives at the `#` operand, not here.
        // Per `MemoryModel.md` § Static analysis rules and
        // `specs/transfer-demotes-to-borrow-spec.md` §2.1.
        auto scope = module->getScopeStack().peek();
        // P3 — definite-assignment check. A local declared without an
        // initializer is in the scope's NYA set until an assignment
        // fires; reading it before then is a compile error.
        if (scope && scope->isNotYetAssigned(identifier)) {
            throw Exception("variable '" + identifier
                + "' may not have been initialized; assign before reading "
                "or declare with an initializer (`= null` for an explicit "
                "null reference, `stack T()` / `heap T()` for an instance)",
                "CAJETA_ERROR_VARIABLE_NOT_ASSIGNED");
        }
        // First: local scope. This is the common path — locals, params,
        // captures.
        FieldPtr field = scope ? scope->getField(identifier) : nullptr;
        if (field) {
            return static_cast<llvm::Value*>(field->getOrCreateAllocation());
        }
        // Implicit-this fallback: emit the equivalent of `this.identifier`
        // when the bare name matches a property of the enclosing class.
        // Loads the `this` pointer from the method's ParameterField, then
        // GEPs into the class struct at the property's slot. The returned
        // value is the field's address (l-value), same shape callers get
        // from a local alloca — l-value-to-r-value coercion uses the
        // ast's resolvedType to load through.
        if (!module->getStructureStack().empty()) {
            auto klass = module->getStructureStack().back();
            if (klass) {
                auto it = klass->getProperties().find(identifier);
                if (it != klass->getProperties().end()) {
                    // Static field shorthand — `a` inside class Two
                    // resolves to Two's static field `a` when `a` is
                    // declared static. Return the global pointer (an
                    // lvalue); the caller load-throughs as needed.
                    // This path also covers P6.2 clinit initializers
                    // where there's no `this` to fall back on.
                    if (it->second->isStatic()) {
                        return static_cast<llvm::Value*>(
                            klass->getOrCreateStaticFieldGlobal(it->second, module));
                    }
                    FieldPtr thisField = scope ? scope->getField("this") : nullptr;
                    if (thisField) {
                        auto* builder = module->getBuilder();
                        auto& ctx = *module->getLlvmContext();
                        llvm::Type* ptrTy = llvm::PointerType::get(ctx, 0);
                        llvm::AllocaInst* thisAlloca = thisField->getOrCreateAllocation();
                        llvm::Value* thisPtr = builder->CreateLoad(ptrTy, thisAlloca);
                        unsigned fieldIdx = (unsigned) klass->getFieldLlvmIndex(it->second);
                        return builder->CreateStructGEP(klass->getLlvmType(),
                            thisPtr, fieldIdx, identifier);
                    }
                    // Matched a non-static instance field, but there is no
                    // `this` in scope (bare reference from a static method or a
                    // clinit). Fail loud instead of returning null IR that
                    // SIGSEGVs downstream.
                    throw Exception(
                        "instance field '" + identifier + "' referenced with no "
                        "receiver ('this' not in scope) — qualify it or make the "
                        "field static", "CAJETA_ERROR_INSTANCE_FIELD_NO_RECEIVER");
                }
            }
        }
        return nullptr;
    }

} // code
