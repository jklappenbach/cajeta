//
// AggregateInitializerExpression — see header for the design.
//

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

    void AggregateInitializerExpression::resolveTypes(CajetaModulePtr module) {
        for (auto& b : bindings) {
            if (b.expression && !b.expression->getResolvedType()) {
                b.expression->resolveTypes(module);
            }
        }
        if (!resolvedType) {
            // Stash the struct type so callers (e.g. LocalVariableDeclaration
            // consulting the initializer's type to size its slot) get the
            // right CajetaType. Falls through to nullptr if the name doesn't
            // resolve — generateCode will surface a clean error below.
            resolvedType = CajetaType::of(typeName);
        }
    }

    llvm::Value* AggregateInitializerExpression::generateCode(CajetaModulePtr module) {
        auto* builder = module->getBuilder();
        auto& ctx = *module->getLlvmContext();

        // Resolve typeName → CajetaClass. View / interface receivers are
        // rejected with specific messages so users get a useful error.
        CajetaTypePtr type = CajetaType::of(typeName);
        if (!type) {
            char buf[256];
            snprintf(buf, sizeof(buf),
                "aggregate initializer for unknown type '%s'",
                typeName.c_str());
            throw locatedException(getSourceLine(), getSourceColumn() + 1, buf,
                "CAJETA_ERROR_AGGREGATE_INIT_UNKNOWN_TYPE");
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
        // P2b — `stack` and `heap` aggregate-init both accept any
        // non-view non-interface class. Vtable initialization is the
        // only thing that differs per type (structs have no vtable in
        // v1; plain CajetaClass does); handled below.
        if (!classType) {
            char buf[256];
            snprintf(buf, sizeof(buf),
                "aggregate initializer `%s { ... }` requires '%s' to be a "
                "class or struct",
                typeName.c_str(), typeName.c_str());
            throw Exception(buf, "CAJETA_ERROR_AGGREGATE_INIT_NOT_CLASS");
        }

        resolvedType = type;
        // Field-index access uses CajetaClass's getFieldLlvmIndex for the
        // plain-class path; views override it to skip the vtable header.
        llvm::Type* bodyTy = classType->getLlvmType();

        // Allocate the body. Stack: entry-block alloca to keep the alloca
        // out of loops + zero-store via Constant null-value (LLVM lowers
        // the same as memset). Heap: malloc + memset zero-init for parity
        // with ClassCreatorRest's heap path.
        llvm::Value* bodyPtr;
        const llvm::DataLayout& dl = module->getLlvmModule()->getDataLayout();
        llvm::Constant* allocSize = llvm::ConstantInt::get(
            llvm::Type::getInt64Ty(ctx),
            dl.getTypeAllocSize(bodyTy));
        if (stackAlloc) {
            bodyPtr = builder->CreateAlloca(bodyTy);
            builder->CreateStore(llvm::Constant::getNullValue(bodyTy), bodyPtr);
        } else {
            bodyPtr = MemoryManager::createMallocInstruction(
                module, allocSize, builder->GetInsertBlock());
            builder->CreateMemSet(bodyPtr,
                llvm::ConstantInt::get(llvm::Type::getInt8Ty(ctx), 0),
                allocSize, llvm::MaybeAlign(8));
        }
        // Initialize vtable pointer at slot 0 — matches the ClassCreatorRest
        // heap path. Applied uniformly to both stack and heap paths so
        // dispatch on an aggregate-init'd class works regardless of storage.
        // Value types (record / @ValueType) have NO slot-0 vtable — writing
        // one would clobber the first field's bytes.
        if (classType->hasVtablePointerAtSlotZero()) {
            if (llvm::GlobalVariable* vt = classType->getVirtualTableGlobal()) {
                llvm::Constant* vtRef = CajetaModule::ensureGlobalInModule(
                    module->getLlvmModule(), vt);
                llvm::Value* vtableSlot = builder->CreateStructGEP(
                    bodyTy, bodyPtr, /*idx=*/0, "vtable_slot");
                builder->CreateStore(vtRef, vtableSlot);
            }
        }
        llvm::Value* bodyAlloca = bodyPtr;  // keep the downstream name

        // Per-binding field stores. Two styles, never mixed (records-spec
        // §4.4): labeled `f: v` binds by name; un-labeled exprs bind
        // POSITIONALLY in declared order (records only — ancestors first,
        // matching the flat layout). Records additionally fill omitted
        // fields from declared defaults and reject omissions without one.
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

        // Evaluate + coerce + store one field. `srcExpr` non-null for user
        // bindings (drives the ownership move-marking); null for defaults.
        auto storeField = [&](const StructurePropertyPtr& prop,
                              llvm::Value* value,
                              const ExpressionPtr& srcExpr) {
            unsigned fieldIdx = (unsigned) classType->getFieldLlvmIndex(prop);
            // generateCode can legitimately yield nullptr (a void / no-value
            // expression); the user-binding call sites pass it straight in.
            // Fail loud instead of dereferencing it below (SIGSEGV).
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
            // Value-type source reached by address (e.g. a field-read GEP):
            // load the aggregate so the store below copies the body, not
            // the pointer, into the inline field slot.
            auto propClass = dynamic_pointer_cast<CajetaClass>(prop->getType());
            llvm::Type* propLlvm = prop->getType()
                ? prop->getType()->getLlvmType() : nullptr;
            if (propClass && propClass->isValueType()
                    && !dynamic_pointer_cast<CajetaView>(prop->getType())
                    && propLlvm && propLlvm->isStructTy()
                    && value && value->getType()->isPointerTy()) {
                value = builder->CreateLoad(propLlvm, value);
            }

            // Coerce the value's LLVM type to the field's declared type
            // (integer-literal int64 → declared int32, etc.). Mirrors the
            // small coercion ladder in StackField::getOrCreateAllocation.
            llvm::Type* fieldTy = prop->getType()->getLlvmType();
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

            // S6.4 + S6.5 ownership transfer for class-ref bindings: the
            // struct now owns the instance — deactivate the source local's
            // drop entry and mark it moved (use-after-move protection).
            // Value-type bindings COPY (inline aggregate, no drop entry) —
            // the source stays live; defaults have no source at all.
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
                        scope->markMoved(idExpr->getTextValue());
                    }
                }
            }
        };

        std::set<const StructureProperty*> boundProps;
        if (anyPositional) {
            // Positional mode (5.2.1): entry i binds orderedFields[i].
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
                // Inherited fields (record static inheritance) live on
                // ancestor property maps — walk supers like field access.
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

        // Records: unbound fields fill from their declared default; a field
        // with neither binding nor default is a compile error (5.1.2).
        // Classes keep the historical zero-fill for omitted fields.
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
