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
    }

    llvm::Value* IdentifierExpression::generateCode(CajetaModulePtr module) {
        // Use-after-move check: if this identifier has been transferred via `#`
        // earlier in the active scope, reject the read at compile time.
        // Per `MemoryModel.md` § Static analysis rules § Use-after-move.
        auto scope = module->getScopeStack().peek();
        if (scope && scope->isMoved(identifier)) {
            throw Exception("use-after-move: identifier '" + identifier
                + "' was transferred via `#` and cannot be read here",
                "CAJETA_ERROR_USE_AFTER_MOVE");
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
                }
            }
        }
        return nullptr;
    }

} // code
