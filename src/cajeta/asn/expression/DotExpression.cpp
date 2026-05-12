//
// Created by James Klappenbach on 4/14/23.
//

#include "DotExpression.h"
#include "../../compile/CajetaModule.h"
#include "../../type/CajetaClass.h"
#include "Identifier.h"

#include <climits>
#include <cmath>

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
    llvm::Value* DotExpression::generateCode(CajetaModulePtr module) {
        if (children.empty()) {
            return nullptr;
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
        }

        llvm::Value* base = children[0]->generateCode(module);
        if (!base) {
            return nullptr;
        }

        auto lhs = dynamic_pointer_cast<Expression>(children[0]);
        if (!lhs) {
            return nullptr;
        }
        auto klass = dynamic_pointer_cast<CajetaClass>(lhs->getResolvedType());
        if (!klass) {
            return nullptr;
        }
        auto it = klass->getProperties().find(identifier);
        if (it == klass->getProperties().end()) {
            return nullptr;
        }
        StructurePropertyPtr property = it->second;
        return module->getBuilder()->CreateStructGEP(klass->getLlvmType(), base,
            property->getOrder(), identifier);
    }

} // code
