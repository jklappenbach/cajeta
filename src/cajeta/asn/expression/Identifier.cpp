// Created by James Klappenbach on 4/14/23.

#include "Identifier.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/error/Exception.h"
#include "../../type/CajetaClass.h"
#include "../../type/StructureProperty.h"

namespace cajeta {
    /// Pins resolvedType to the named field's type, from the active scope or,
    /// failing that, from a property of the enclosing class. A bare CLASS NAME
    /// deliberately stays unresolved; see the note at the end of the body.
    void IdentifierExpression::resolveTypes(CajetaModulePtr module) {
        if (!module->getScopeStack().isEmpty()) {
            FieldPtr field = module->getScopeStack().peek()->getField(identifier);
            if (field) {
                resolvedType = field->getType();
                return;
            }
        }
        // Implicit-this: a bare `x` may be shorthand for `this.x`.
        if (!module->getStructureStack().empty()) {
            auto klass = module->getStructureStack().back();
            if (klass) {
                auto it = klass->getProperties().find(identifier);
                if (it != klass->getProperties().end()) {
                    resolvedType = it->second->getType();
                }
            }
        }
        // A class-name identifier is deliberately left unresolved:
        // MethodReferenceExpression tells `myInstance::next` from `Counter::next`
        // by whether the LHS has a resolvedType, and pinning one collapses that.
    }

    /// Returns the identifier's ADDRESS: a local's alloca, a static's global,
    /// or a GEP through `this`. A transferred binding is still readable here,
    /// since `#` demotes its source to a borrow; the re-transfer check is at `#`.
    llvm::Value* IdentifierExpression::generateCode(CajetaModulePtr module) {
        auto scope = module->getScopeStack().peek();
        if (scope && scope->isNotYetAssigned(identifier)) {
            throw Exception("variable '" + identifier
                + "' may not have been initialized; assign before reading "
                "or declare with an initializer (`= null` for an explicit "
                "null reference, `stack T()` / `heap T()` for an instance)",
                "CAJETA_ERROR_VARIABLE_NOT_ASSIGNED");
        }
        FieldPtr field = scope ? scope->getField(identifier) : nullptr;
        if (field) {
            // A binding seeded from an earlier session unit: a moved-out one is
            // rejected, its borrow validity being invisible across the seam.
            if (field->isSessionSeeded()) {
                if (scope->isBorrow(identifier)) {
                    string note = scope->transferSiteOf(identifier);
                    throw Exception(
                        "session binding `" + identifier + "` was moved out "
                        "in an earlier unit"
                        + (note.empty() ? "" : " (" + note + ")")
                        + "; rebind it (`" + identifier + " = ...`) before "
                        "reading",
                        "CAJETA_ERROR_MOVE_OF_BORROW",
                        module->getScriptHostName(), getSourceLine(),
                        getSourceColumn());
                }
                // The binding lives in the runtime session registry, not this
                // frame, so it is staged into a local slot on EVERY access: an
                // earlier statement here may have rebound and dropped it.
                auto* builder = module->getBuilder();
                llvm::Function* getFn =
                    module->getRuntimeFunction("__cajeta_session_get");
                llvm::AllocaInst* slot = field->getOrCreateAllocation();
                if (getFn && slot) {
                    llvm::Value* nameStr =
                        builder->CreateGlobalString(identifier);
                    llvm::Value* live = builder->CreateCall(getFn, {nameStr});
                    auto& lctx = *module->getLlvmContext();
                    llvm::Type* slotTy = slot->getAllocatedType();
                    (void) lctx;
                    (void) slotTy;
                    // Discriminated on the FIELD'S type, as seedSessionScope was.
                    CajetaTypePtr ft = field->getType();
                    bool primitive = ft && (ft->getTypeFlags() & PRIMITIVE_FLAG);
                    if (primitive) {
                        // A primitive is BOXED on bind, so the registry returns
                        // the value's ADDRESS. The box pointer cannot be returned
                        // directly: consumers read through the field's own slot.
                        llvm::Value* loaded =
                            builder->CreateLoad(slot->getAllocatedType(), live);
                        builder->CreateStore(loaded, slot);
                        return static_cast<llvm::Value*>(slot);
                    }
                    builder->CreateStore(live, slot);
                }
                return static_cast<llvm::Value*>(slot);
            }
            return static_cast<llvm::Value*>(field->getOrCreateAllocation());
        }
        // Implicit-this: emit `this.identifier`, returning the field's ADDRESS.
        if (!module->getStructureStack().empty()) {
            auto klass = module->getStructureStack().back();
            if (klass) {
                auto it = klass->getProperties().find(identifier);
                if (it != klass->getProperties().end()) {
                    // A static also takes this path in a clinit initializer,
                    // where there is no `this` to fall back on.
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
                    // No `this` in scope: fail loud, not with null IR.
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
