//
// AggregateInitializerExpression — see header for the design.
//

#include "AggregateInitializerExpression.h"
#include "../../compile/CajetaModule.h"
#include "../../type/CajetaStruct.h"
#include "../../type/CajetaView.h"
#include "../../type/CajetaClass.h"
#include "../../error/Exception.h"

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

        // Resolve typeName → CajetaStruct. View / plain-class receivers are
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
        auto structType = dynamic_pointer_cast<CajetaStruct>(type);
        if (!structType) {
            char buf[256];
            snprintf(buf, sizeof(buf),
                "aggregate initializer `%s { ... }` requires '%s' to be a "
                "struct; got a class or interface",
                typeName.c_str(), typeName.c_str());
            throw Exception(buf, "CAJETA_ERROR_AGGREGATE_INIT_NOT_STRUCT");
        }

        resolvedType = type;
        llvm::Type* bodyTy = structType->getLlvmType();

        // Stack-allocate the struct body and zero-init so any field the
        // initializer omits lands at 0 / null. The zero store also covers
        // any LLVM-inserted padding between fields, which matters for
        // memcmp-style equality and for predictable hashes.
        llvm::Value* bodyAlloca = builder->CreateAlloca(bodyTy);
        builder->CreateStore(llvm::Constant::getNullValue(bodyTy), bodyAlloca);

        // Per-binding field stores. v1 requires labels; positional init is
        // out of scope (Structs.md uses the labeled form too, and label-less
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
            auto& props = structType->getProperties();
            auto it = props.find(b.label);
            if (it == props.end()) {
                char buf[512];
                snprintf(buf, sizeof(buf),
                    "aggregate initializer for '%s' names field '%s' that the "
                    "struct does not declare",
                    typeName.c_str(), b.label.c_str());
                throw Exception(buf, "CAJETA_ERROR_AGGREGATE_INIT_UNKNOWN_FIELD");
            }
            StructurePropertyPtr prop = it->second;
            unsigned fieldIdx = (unsigned) structType->getFieldLlvmIndex(prop);

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
        }

        return bodyAlloca;
    }

} // namespace cajeta
