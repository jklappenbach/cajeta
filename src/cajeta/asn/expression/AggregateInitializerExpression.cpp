// AggregateInitializerExpression — see header for the design.

#include "AggregateInitializerExpression.h"
#include "Identifier.h"
#include "../../compile/CajetaModule.h"
#include "../../error/Diagnostics.h"
#include "../../type/CajetaView.h"
#include "../../type/CajetaArray.h"
#include "../../type/CajetaClass.h"
#include "../../type/Scope.h"
#include "../../field/Field.h"
#include "../../error/Exception.h"
#include "../../util/MemoryManager.h"

#include <set>

namespace cajeta {

    /// Resolves the binding expressions and the literal's own type: an explicit
    /// `typeName` prefix, else the context-pushed expectedType. May leave
    /// resolvedType null; generateCode re-resolves and reports the error.
    void AggregateInitializerExpression::resolveTypes(CajetaModulePtr module) {
        for (auto& b : bindings) {
            if (b.expression && !b.expression->getResolvedType()) {
                b.expression->resolveTypes(module);
            }
        }
        if (!resolvedType) {
            resolvedType = !typeName.empty() ? CajetaType::of(typeName)
                                             : expectedType;
        }
    }

    /// Emits the aggregate literal: allocate (stack alloca or heap malloc),
    /// zero, install the vtable, then store each binding. Returns the body
    /// pointer. Throws a located Exception for every malformed initializer.
    llvm::Value* AggregateInitializerExpression::generateCode(CajetaModulePtr module) {
        auto* builder = module->getBuilder();
        auto& ctx = *module->getLlvmContext();

        CajetaTypePtr type = !typeName.empty() ? CajetaType::of(typeName)
                                               : expectedType;
        if (!type && typeName.empty()) {
            throw locatedException(getSourceLine(), getSourceColumn() + 1,
                "aggregate literal `{ ... }` has no inferable type here; give "
                "it a type prefix (e.g. `Point { ... }`) or use it where a "
                "class type is expected (a typed declaration, assignment, "
                "return, or array element)",
                "CAJETA_ERROR_AGGREGATE_INIT_NO_TYPE");
        }
        if (!type) {
            char buf[256];
            snprintf(buf, sizeof(buf),
                "aggregate initializer for unknown type '%s'",
                typeName.c_str());
            throw locatedException(getSourceLine(), getSourceColumn() + 1, buf,
                "CAJETA_ERROR_AGGREGATE_INIT_UNKNOWN_TYPE");
        }
        // Backfill typeName so the per-field diagnostics below name the target.
        if (typeName.empty() && type->getQName()) {
            typeName = type->getQName()->getTypeName();
        }
        if (dynamic_pointer_cast<CajetaView>(type)) {
            char buf[256];
            snprintf(buf, sizeof(buf),
                "aggregate initializer syntax `%s { ... }` is not supported for "
                "views; construct a view with `%s(byteBuffer)` instead",
                typeName.c_str(), typeName.c_str());
            throw Exception(buf, "CAJETA_ERROR_AGGREGATE_INIT_ON_VIEW");
        }
        auto classType  = dynamic_pointer_cast<CajetaClass>(type);
        if (classType && classType->isInterface()) {
            char buf[256];
            snprintf(buf, sizeof(buf),
                "aggregate initializer `%s { ... }` cannot target an "
                "interface; provide a concrete implementer",
                typeName.c_str());
            throw Exception(buf, "CAJETA_ERROR_AGGREGATE_INIT_ON_INTERFACE");
        }
        if (!classType) {
            char buf[256];
            snprintf(buf, sizeof(buf),
                "aggregate initializer `%s { ... }` requires '%s' to be a "
                "class or struct",
                typeName.c_str(), typeName.c_str());
            throw Exception(buf, "CAJETA_ERROR_AGGREGATE_INIT_NOT_CLASS");
        }

        resolvedType = type;
        llvm::Type* bodyTy = classType->getLlvmType();

        llvm::Value* bodyPtr;
        const llvm::DataLayout& dl = module->getLlvmModule()->getDataLayout();
        llvm::Constant* allocSize = llvm::ConstantInt::get(
            llvm::Type::getInt64Ty(ctx),
            dl.getTypeAllocSize(bodyTy));
        if (stackAlloc) {
            // The alloca goes in the ENTRY block, not the current insert point:
            // in a loop body that reuses one slot instead of one per iteration.
            llvm::Function* curFn = builder->GetInsertBlock()->getParent();
            llvm::BasicBlock& entryBB = curFn->getEntryBlock();
            llvm::IRBuilder<> entryB(&entryBB, entryBB.getFirstInsertionPt());
            bodyPtr = entryB.CreateAlloca(bodyTy);
            builder->CreateStore(llvm::Constant::getNullValue(bodyTy), bodyPtr);
        } else {
            bodyPtr = MemoryManager::createMallocInstruction(
                module, allocSize, builder->GetInsertBlock());
            builder->CreateMemSet(bodyPtr,
                llvm::ConstantInt::get(llvm::Type::getInt8Ty(ctx), 0),
                allocSize, llvm::MaybeAlign(8));
        }
        // Value types (record / @ValueType) have NO slot-0 vtable; storing one
        // would clobber the first field's bytes.
        if (classType->hasVtablePointerAtSlotZero()) {
            if (llvm::GlobalVariable* vt = classType->getVirtualTableGlobal()) {
                llvm::Constant* vtRef = CajetaModule::ensureGlobalInModule(
                    module->emitTargetLlvmModule(), vt);
                llvm::Value* vtableSlot = builder->CreateStructGEP(
                    bodyTy, bodyPtr, /*idx=*/0, "vtable_slot");
                builder->CreateStore(vtRef, vtableSlot);
            }
        }
        llvm::Value* bodyAlloca = bodyPtr;

        llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
        bool isRecord = classType->isRecordType();
        bool anyLabeled = false;
        bool anyPositional = false;
        for (auto& b : bindings) {
            (b.label.empty() ? anyPositional : anyLabeled) = true;
        }
        if (anyLabeled && anyPositional) {
            char buf[256];
            snprintf(buf, sizeof(buf),
                "aggregate initializer for '%s' mixes labeled and positional "
                "bindings; use one style per initializer",
                typeName.c_str());
            throw Exception(buf, "CAJETA_ERROR_AGGREGATE_INIT_MIXED");
        }
        if (anyPositional && !isRecord) {
            char buf[256];
            snprintf(buf, sizeof(buf),
                "aggregate initializer for '%s' uses a positional binding; "
                "labeled fields are required (e.g. `field: value`)",
                typeName.c_str());
            throw Exception(buf, "CAJETA_ERROR_AGGREGATE_INIT_UNLABELED");
        }

        // Declared-order instance fields, ancestors first (flat layout order).
        vector<StructurePropertyPtr> orderedFields;
        std::function<void(const CajetaClassPtr&)> collectFields =
            [&](const CajetaClassPtr& cls) {
                if (!cls) return;
                for (auto& sup : cls->getSuperClasses()) collectFields(sup);
                for (auto& pr : cls->getPropertyList()) {
                    if (pr && !pr->isStatic()) orderedFields.push_back(pr);
                }
            };
        collectFields(classType);

        // Evaluate, coerce and store one field. `srcExpr` is the user binding
        // that drives ownership move-marking, and is null for defaults.
        auto storeField = [&](const StructurePropertyPtr& prop,
                              llvm::Value* value,
                              const ExpressionPtr& srcExpr) {
            unsigned fieldIdx = (unsigned) classType->getFieldLlvmIndex(prop);
            if (!value) {
                char buf[512];
                snprintf(buf, sizeof(buf),
                    "aggregate initializer for '%s': the expression bound to "
                    "field '%s' produces no value",
                    typeName.c_str(), prop->getName().c_str());
                throw Exception(buf, "CAJETA_ERROR_AGGREGATE_INIT_TYPE");
            }
            if (auto* a = llvm::dyn_cast_or_null<llvm::AllocaInst>(value)) {
                value = builder->CreateLoad(a->getAllocatedType(), a);
            }
            // A value-type source reached by address must be loaded, so the
            // store below copies the body into the inline slot, not a pointer.
            auto propClass = dynamic_pointer_cast<CajetaClass>(prop->getType());
            llvm::Type* propLlvm = prop->getType()
                ? prop->getType()->getLlvmType() : nullptr;
            if (propClass && propClass->isValueType()
                    && !dynamic_pointer_cast<CajetaView>(prop->getType())
                    && propLlvm && propLlvm->isStructTy()
                    && value && value->getType()->isPointerTy()) {
                value = builder->CreateLoad(propLlvm, value);
            }

            llvm::Type* fieldTy = prop->getType()->getLlvmType();
            // ARRAY fields only: `T[]` reports its `{i64,[0 x T]}` struct here
            // while its slot is a plain pointer. Widening this to every field
            // makes a wrong class-typed binding coerce pointer-to-pointer.
            if (dynamic_pointer_cast<CajetaArray>(prop->getType())) {
                if (auto* bodyStruct = llvm::dyn_cast<llvm::StructType>(bodyTy)) {
                    if (fieldIdx < bodyStruct->getNumElements()) {
                        fieldTy = bodyStruct->getElementType(fieldIdx);
                    }
                }
            }
            if (value->getType() != fieldTy) {
                llvm::Type* srcTy = value->getType();
                if (fieldTy->isIntegerTy() && srcTy->isIntegerTy()) {
                    value = builder->CreateIntCast(value, fieldTy, /*isSigned=*/true);
                } else if (fieldTy->isFloatingPointTy() && srcTy->isFloatingPointTy()) {
                    value = builder->CreateFPCast(value, fieldTy);
                } else if (fieldTy->isFloatingPointTy() && srcTy->isIntegerTy()) {
                    value = builder->CreateSIToFP(value, fieldTy);
                } else if (fieldTy->isIntegerTy() && srcTy->isFloatingPointTy()) {
                    value = builder->CreateFPToSI(value, fieldTy);
                } else if (fieldTy->isAggregateType()) {
                    char buf[512];
                    snprintf(buf, sizeof(buf),
                        "aggregate initializer for '%s': value bound to field "
                        "'%s' does not match the field's type",
                        typeName.c_str(), prop->getName().c_str());
                    throw Exception(buf, "CAJETA_ERROR_AGGREGATE_INIT_TYPE");
                }
            }

            vector<llvm::Value*> gepIndices = {
                llvm::ConstantInt::get(i32Ty, 0),
                llvm::ConstantInt::get(i32Ty, fieldIdx),
            };
            llvm::Value* slot = builder->CreateInBoundsGEP(
                bodyTy, bodyAlloca, gepIndices, "agg_field_" + prop->getName());
            builder->CreateStore(value, slot);

            // A class-ref binding transfers: the aggregate owns the instance,
            // so the source's drop entry is deactivated and it becomes a
            // borrow. Value-type bindings copy, and their source stays live.
            bool fieldIsClassRef = propClass != nullptr
                && !dynamic_pointer_cast<CajetaView>(prop->getType())
                && !dynamic_pointer_cast<CajetaArray>(prop->getType())
                && !propClass->isInterface()
                && !propClass->isValueType();
            if (fieldIsClassRef && srcExpr) {
                if (auto idExpr = dynamic_pointer_cast<IdentifierExpression>(srcExpr)) {
                    auto scope = module->getScopeStack().peek();
                    if (scope) {
                        FieldPtr srcField = scope->getField(idExpr->getTextValue());
                        if (srcField) {
                            if (llvm::Value* entry = srcField->getDropEntry()) {
                                if (llvm::Function* mark = module->getRuntimeFunction(
                                        "__cajeta_drop_mark_inactive")) {
                                    builder->CreateCall(mark, {entry});
                                }
                            }
                        }
                        scope->demoteToBorrow(idExpr->getTextValue());
                    }
                }
            }
        };

        std::set<const StructureProperty*> boundProps;
        if (anyPositional) {
            if (bindings.size() > orderedFields.size()) {
                char buf[512];
                snprintf(buf, sizeof(buf),
                    "aggregate initializer for '%s' has %zu positional values "
                    "but the record declares %zu field(s)",
                    typeName.c_str(), bindings.size(), orderedFields.size());
                throw Exception(buf, "CAJETA_ERROR_AGGREGATE_INIT_ARITY");
            }
            for (size_t i = 0; i < bindings.size(); ++i) {
                auto& b = bindings[i];
                auto& prop = orderedFields[i];
                if (!b.expression->getResolvedType()) {
                    b.expression->resolveTypes(module);
                }
                llvm::Value* value = b.expression->generateCode(module);
                storeField(prop, value, b.expression);
                boundProps.insert(prop.get());
            }
        } else {
            for (auto& b : bindings) {
                // Inherited fields live on ancestor property maps.
                StructurePropertyPtr prop;
                std::function<bool(const CajetaClassPtr&)> findProp =
                    [&](const CajetaClassPtr& cls) -> bool {
                        if (!cls) return false;
                        auto pit = cls->getProperties().find(b.label);
                        if (pit != cls->getProperties().end()) {
                            prop = pit->second;
                            return true;
                        }
                        for (auto& sup : cls->getSuperClasses()) {
                            if (findProp(sup)) return true;
                        }
                        return false;
                    };
                findProp(classType);
                if (!prop) {
                    char buf[512];
                    snprintf(buf, sizeof(buf),
                        "aggregate initializer for '%s' names field '%s' that the "
                        "type does not declare",
                        typeName.c_str(), b.label.c_str());
                    throw Exception(buf, "CAJETA_ERROR_AGGREGATE_INIT_UNKNOWN_FIELD");
                }
                if (!b.expression->getResolvedType()) {
                    b.expression->resolveTypes(module);
                }
                llvm::Value* value = b.expression->generateCode(module);
                storeField(prop, value, b.expression);
                boundProps.insert(prop.get());
            }
        }

        // Records fill unbound fields from their declared default and reject
        // an omission with none; classes zero-fill omitted fields instead.
        if (isRecord) {
            for (auto& prop : orderedFields) {
                if (boundProps.count(prop.get())) continue;
                auto init = prop->getInitializer();
                if (!init) {
                    char buf[512];
                    snprintf(buf, sizeof(buf),
                        "aggregate initializer for '%s' omits field '%s', "
                        "which has no declared default",
                        typeName.c_str(), prop->getName().c_str());
                    throw Exception(buf,
                        "CAJETA_ERROR_AGGREGATE_INIT_MISSING_FIELD");
                }
                llvm::Value* value = init->generateCode(module);
                if (!value) {
                    char buf[512];
                    snprintf(buf, sizeof(buf),
                        "aggregate initializer for '%s': default for field "
                        "'%s' produced no value",
                        typeName.c_str(), prop->getName().c_str());
                    throw Exception(buf,
                        "CAJETA_ERROR_AGGREGATE_INIT_MISSING_FIELD");
                }
                storeField(prop, value, nullptr);
            }
        }

        return bodyAlloca;
    }

} // namespace cajeta
