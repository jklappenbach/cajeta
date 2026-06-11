//
// AggregateInitializerExpression — see header for the design.
//

#include "AggregateInitializerExpression.h"
#include "Identifier.h"
#include "../../compile/CajetaModule.h"
#include "../../type/CajetaView.h"
#include "../../type/CajetaArray.h"
#include "../../type/CajetaClass.h"
#include "../../type/Scope.h"
#include "../../field/Field.h"
#include "../../error/Exception.h"
#include "../../util/MemoryManager.h"

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
            throw Exception(buf, "CAJETA_ERROR_AGGREGATE_INIT_UNKNOWN_TYPE");
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
        if (llvm::GlobalVariable* vt = classType->getVirtualTableGlobal()) {
            llvm::Constant* vtRef = CajetaModule::ensureGlobalInModule(
                module->getLlvmModule(), vt);
            llvm::Value* vtableSlot = builder->CreateStructGEP(
                bodyTy, bodyPtr, /*idx=*/0, "vtable_slot");
            builder->CreateStore(vtRef, vtableSlot);
        }
        llvm::Value* bodyAlloca = bodyPtr;  // keep the downstream name

        // Per-binding field stores. v1 requires labels; positional init is
        // out of scope (docs/stdlib/Views.md uses the labeled form too, and label-less
        // init would silently couple field order to declaration order).
        llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
        for (auto& b : bindings) {
            if (b.label.empty()) {
                char buf[256];
                snprintf(buf, sizeof(buf),
                    "aggregate initializer for '%s' uses a positional binding; "
                    "labeled fields are required (e.g. `field: value`)",
                    typeName.c_str());
                throw Exception(buf, "CAJETA_ERROR_AGGREGATE_INIT_UNLABELED");
            }
            // getFieldLlvmIndex dispatches via the virtual override:
            // CajetaView skips the vtable header; CajetaClass adds it.
            auto& props = classType->getProperties();
            auto it = props.find(b.label);
            if (it == props.end()) {
                char buf[512];
                snprintf(buf, sizeof(buf),
                    "aggregate initializer for '%s' names field '%s' that the "
                    "type does not declare",
                    typeName.c_str(), b.label.c_str());
                throw Exception(buf, "CAJETA_ERROR_AGGREGATE_INIT_UNKNOWN_FIELD");
            }
            StructurePropertyPtr prop = it->second;
            unsigned fieldIdx = (unsigned) classType->getFieldLlvmIndex(prop);

            // Evaluate the binding expression; load through if it's still
            // an alloca slot (same coercion the StackField initializer
            // path runs for primitives).
            if (!b.expression->getResolvedType()) {
                b.expression->resolveTypes(module);
            }
            llvm::Value* value = b.expression->generateCode(module);
            if (auto* a = llvm::dyn_cast_or_null<llvm::AllocaInst>(value)) {
                value = builder->CreateLoad(a->getAllocatedType(), a);
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
                }
            }

            vector<llvm::Value*> gepIndices = {
                llvm::ConstantInt::get(i32Ty, 0),
                llvm::ConstantInt::get(i32Ty, fieldIdx),
            };
            llvm::Value* slot = builder->CreateInBoundsGEP(
                bodyTy, bodyAlloca, gepIndices, "agg_field_" + b.label);
            builder->CreateStore(value, slot);

            // S6.4 + S6.5 ownership transfer for class-ref bindings.
            // When the binding sources a class instance from a local
            // identifier, the struct now owns it — two side effects:
            //
            //   (S6.4) Deactivate the source's drop entry so only the
            //          struct's drop fn frees the instance; without this
            //          both the source local and the struct would call
            //          drop, double-freeing at scope exit.
            //
            //   (S6.5) Mark the source identifier as moved in the current
            //          scope. Subsequent reads (e.g. `tag.n` after `Holder
            //          { t: tag }`) trip CAJETA_ERROR_USE_AFTER_MOVE,
            //          closing the soundness gap from S6.4 limitation #1:
            //          the struct now owns the instance and may free it
            //          via its drop chain at any point past this line.
            //
            // v1 simplification: every class-ref binding is treated as a
            // move regardless of whether the user wrote `#` on the RHS.
            // Borrow-form bindings (struct field holding a borrowed class
            // ref whose drop the struct must skip) still need per-instance
            // ownership tracking and remain deferred.
            auto fieldClass = dynamic_pointer_cast<CajetaClass>(prop->getType());
            bool fieldIsClassRef = fieldClass != nullptr
                && !dynamic_pointer_cast<CajetaView>(prop->getType())
                && !dynamic_pointer_cast<CajetaArray>(prop->getType())
                && !fieldClass->isInterface();
            if (fieldIsClassRef) {
                if (auto idExpr = dynamic_pointer_cast<IdentifierExpression>(b.expression)) {
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
        }

        return bodyAlloca;
    }

} // namespace cajeta
