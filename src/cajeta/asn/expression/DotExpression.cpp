//
// Created by James Klappenbach on 4/14/23.
//

#include "DotExpression.h"
#include "../../compile/CajetaModule.h"
#include "../../type/CajetaClass.h"
#include "../../type/CajetaStruct.h"
#include "../../error/Exception.h"
#include "Identifier.h"

#include <climits>
#include <cmath>
#include <llvm/IR/Intrinsics.h>

namespace cajeta {
    DotExpression::DotExpression(CajetaParser::ExpressionContext* ctx, antlr4::Token* token) : Expression(token) {
        // The DOT grammar allows several rhs forms (identifier, methodCall, THIS, etc.).
        // Only the identifier form is fully implemented; for other forms we capture an
        // empty name and rely on the lhs's codegen to surface the right error.
        if (ctx->identifier()) {
            identifier = ctx->identifier()->getText();
        }
    }

    void DotExpression::resolveTypes(CajetaModulePtr module) {
        // Resolve the LHS (instance expression) first, then look up our member on its
        // resolved class type and pin our own type to the member's type.
        AbstractSyntaxNode::resolveTypes(module);
        if (children.empty()) {
            return;
        }
        auto lhs = dynamic_pointer_cast<Expression>(children[0]);
        if (!lhs) {
            return;
        }
        // Enum constant reference: `MyEnum.NAME` — resolvedType is int32.
        // Bind here so diamond inference and other resolveType consumers see
        // the right type without waiting for generateCode to short-circuit.
        if (auto id = dynamic_pointer_cast<IdentifierExpression>(lhs)) {
            const string& ns = id->getTextValue();
            if (CajetaType::lookupEnumConstant(ns, identifier).has_value()) {
                resolvedType = CajetaType::of("int32");
                return;
            }
        }
        auto klass = dynamic_pointer_cast<CajetaClass>(lhs->getResolvedType());
        if (!klass) {
            return;
        }
        auto it = klass->getProperties().find(identifier);
        if (it != klass->getProperties().end()) {
            resolvedType = it->second->getType();
        }
    }

    // `a.b` lowers to a struct GEP. lhs may be the alloca holding the struct (stack-local)
    // or a pointer loaded from the heap; both yield an address we can GEP into. The member
    // index comes from StructureProperty::getOrder() — set when the class registered its
    // properties during signature pass.
    llvm::Value* DotExpression::maybeBswap(CajetaModulePtr module, llvm::Value* v,
                                              const ExpressionPtr& receiver) {
        if (!v || !receiver) return v;
        auto recvType = receiver->getResolvedType();
        if (!recvType) return v;
        auto structType = dynamic_pointer_cast<CajetaStruct>(recvType);
        if (!structType) return v;
        StructEndianness e = structType->getEndianness();
        if (e == StructEndianness::Host) return v;
        // v1 assumption: host is little-endian (x86_64, aarch64). When we
        // grow cross-compile support, this picks the host's order from the
        // target triple instead.
        const bool hostLittle = true;
        bool needBswap = (e == StructEndianness::Big && hostLittle)
                      || (e == StructEndianness::Little && !hostLittle);
        if (!needBswap) return v;
        llvm::Type* t = v->getType();
        if (!t->isIntegerTy()) return v;          // float bswap is post-v1
        if (t->getIntegerBitWidth() <= 8) return v;  // single byte has no byte order
        llvm::Function* fn = llvm::Intrinsic::getDeclaration(
            module->getLlvmModule(), llvm::Intrinsic::bswap, {t});
        return module->getBuilder()->CreateCall(fn, {v});
    }

    string DotExpression::buildPath(const ExpressionPtr& expr) {
        if (auto id = dynamic_pointer_cast<IdentifierExpression>(expr)) {
            return id->getTextValue();
        }
        if (auto dot = dynamic_pointer_cast<DotExpression>(expr)) {
            const auto& children = const_cast<DotExpression*>(dot.get())->getChildren();
            if (children.empty()) return "";
            auto lhs = dynamic_pointer_cast<Expression>(children[0]);
            if (!lhs) return "";
            string lhsPath = buildPath(lhs);
            if (lhsPath.empty()) return "";
            return lhsPath + "." + dot->getIdentifier();
        }
        return "";
    }

    llvm::Value* DotExpression::generateCode(CajetaModulePtr module) {
        if (children.empty()) {
            return nullptr;
        }

        // Use-after-move check for field-access paths. Build the dotted path
        // from this DotExpression down to its named root, then ask the scope
        // whether any prefix of that path has been moved-out. This catches
        // patterns like `#person.name` followed by a read of `person.name`
        // or any path through it.
        //
        // Per MemoryModel.md § Path-based borrow tracking. Skip when the LHS
        // doesn't bottom out at a named identifier (e.g. `factory.make().foo`)
        // — those produce anonymous owners and are handled by the separate
        // anonymous-owner rule.
        {
            ExpressionPtr self = dynamic_pointer_cast<Expression>(shared_from_this());
            string path = buildPath(self);
            if (!path.empty()) {
                auto scope = module->getScopeStack().peek();
                if (scope && scope->isPathMoved(path)) {
                    throw Exception("use-after-move: path '" + path
                        + "' was transferred via `#` and cannot be read here",
                        "CAJETA_ERROR_USE_AFTER_MOVE");
                }
            }
        }

        // Static-namespace constants: Math.PI / Math.E / Integer.MAX_VALUE / ... .
        // These have no instance backing and don't survive the GEP path below, so we
        // short-circuit them here and emit IR constants directly.
        if (auto idExpr = dynamic_pointer_cast<IdentifierExpression>(children[0])) {
            auto& ctx = *module->getLlvmContext();
            const std::string& ns = idExpr->getTextValue();
            if (ns == "Math") {
                if (identifier == "PI") return llvm::ConstantFP::get(
                    llvm::Type::getDoubleTy(ctx), M_PI);
                if (identifier == "E")  return llvm::ConstantFP::get(
                    llvm::Type::getDoubleTy(ctx), M_E);
            } else if (ns == "Integer") {
                if (identifier == "MAX_VALUE") return llvm::ConstantInt::get(
                    llvm::Type::getInt32Ty(ctx), INT32_MAX, /*isSigned=*/true);
                if (identifier == "MIN_VALUE") return llvm::ConstantInt::get(
                    llvm::Type::getInt32Ty(ctx), INT32_MIN, /*isSigned=*/true);
            } else if (ns == "Long") {
                if (identifier == "MAX_VALUE") return llvm::ConstantInt::get(
                    llvm::Type::getInt64Ty(ctx), INT64_MAX, /*isSigned=*/true);
                if (identifier == "MIN_VALUE") return llvm::ConstantInt::get(
                    llvm::Type::getInt64Ty(ctx), INT64_MIN, /*isSigned=*/true);
            }
            // Enum constant: `MyEnum.NAME` resolves to the ordinal i32. The
            // enum type is registered in canonicalMap; the constant table is
            // a separate side-map populated by visitEnumDeclaration.
            if (auto v = CajetaType::lookupEnumConstant(ns, identifier)) {
                return llvm::ConstantInt::get(
                    llvm::Type::getInt32Ty(ctx), *v, /*isSigned=*/true);
            }
        }

        llvm::Value* base = children[0]->generateCode(module);
        if (!base) {
            return nullptr;
        }

        auto lhs = dynamic_pointer_cast<Expression>(children[0]);
        if (!lhs) {
            return nullptr;
        }
        // Re-run resolveTypes if the lhs wasn't resolved during the pre-pass —
        // local variables aren't added to the scope until their declarations
        // run at codegen time, so identifiers referenced later may have a
        // null resolvedType at resolve time.
        if (!lhs->getResolvedType()) {
            lhs->resolveTypes(module);
        }
        auto klass = dynamic_pointer_cast<CajetaClass>(lhs->getResolvedType());
        if (!klass) {
            return nullptr;
        }
        auto it = klass->getProperties().find(identifier);
        if (it == klass->getProperties().end()) {
            return nullptr;
        }
        // If the receiver is an l-value (an alloca that holds a pointer to the
        // object), load through it first. The struct/class instance lives at
        // the address the alloca stores; GEP'ing the alloca directly would
        // walk the slot, not the object.
        if (auto* a = llvm::dyn_cast<llvm::AllocaInst>(base)) {
            base = module->getBuilder()->CreateLoad(a->getAllocatedType(), a);
        }
        StructurePropertyPtr property = it->second;
        // Set our own resolvedType so callers can load-through with the right
        // element type. The pre-pass resolveTypes can't always determine this
        // (locals aren't in scope until their declarations run at codegen).
        resolvedType = property->getType();
        // Field index depends on the receiver type. CajetaClass instances
        // reserve LLVM slot 0 for the vtable pointer, so user fields land at
        // index getOrder()+1. CajetaStruct (POD) uses getOrder() directly.
        unsigned fieldIdx = (unsigned) klass->getFieldLlvmIndex(property);

        // Variable-size struct fields (Session 5.5b): the LLVM struct holds
        // only the i32 length prefix at this slot; the data bytes live past
        // the struct's footprint in the buffer. Emit specialized codegen:
        //   1. Load length from the prefix slot.
        //   2. GEP past the LLVM struct to the data start.
        //   3. Call __cajeta_str_view_to_owned to materialize a null-
        //      terminated owned copy that's compatible with the String stdlib.
        // The result is a fresh String pointer; callers that store it must
        // either own it (and free at scope-end) or use it transiently.
        if (CajetaStruct::isVariableSize(property)) {
            auto* builder = module->getBuilder();
            auto& ctx = *module->getLlvmContext();
            llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
            llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
            llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);

            llvm::Value* lenPrefixPtr = builder->CreateStructGEP(
                klass->getLlvmType(), base, fieldIdx, identifier + "_len_ptr");
            llvm::Value* length = builder->CreateLoad(i32Ty, lenPrefixPtr, identifier + "_len");
            llvm::Value* length64 = builder->CreateIntCast(length, i64Ty, /*isSigned=*/true);

            // Data starts immediately after the LLVM struct. Use the struct's
            // total byte size (DataLayout) as the offset.
            const llvm::DataLayout& dl = module->getLlvmModule()->getDataLayout();
            uint64_t structBytes = dl.getTypeAllocSize(klass->getLlvmType());
            llvm::Value* dataPtr = builder->CreateInBoundsGEP(
                i8Ty, base, llvm::ConstantInt::get(i64Ty, structBytes), identifier + "_data");

            llvm::Function* fn = module->getRuntimeFunction("__cajeta_str_view_to_owned");
            if (!fn) return nullptr;
            return builder->CreateCall(fn, {dataPtr, length64});
        }
        return module->getBuilder()->CreateStructGEP(klass->getLlvmType(), base,
            fieldIdx, identifier);
    }

} // code
